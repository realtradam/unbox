#include "file_watcher.hpp"

// inotify is libc (NOT wlroots), so it does not go through wlr.hpp. Integrated
// into the kernel's wl_event_loop via wl_event_loop_add_fd — never blocks.
#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>

namespace unbox::kernel {

FileWatcher::FileWatcher(wl_event_loop* loop, std::function<void(ExtensionId)> disable)
    : loop_(loop), disable_(std::move(disable)) {}

FileWatcher::~FileWatcher() {
    // Tear down before the loop/display dies: remove the event source, then
    // close the fd (closing drops every inotify watch).
    if (source_ != nullptr) {
        wl_event_source_remove(source_);
        source_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

auto FileWatcher::dispatch(int /*fd*/, std::uint32_t /*mask*/, void* data) -> int {
    static_cast<FileWatcher*>(data)->on_readable();
    return 0;
}

bool FileWatcher::ensure_started() {
    if (fd_ >= 0) {
        return true;
    }
    if (loop_ == nullptr) {
        return false;
    }
    fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd_ < 0) {
        wlr_log(WLR_ERROR, "file-watcher: inotify_init1 failed; watching disabled");
        return false;
    }
    source_ = wl_event_loop_add_fd(loop_, fd_, WL_EVENT_READABLE, dispatch, this);
    if (source_ == nullptr) {
        close(fd_);
        fd_ = -1;
        wlr_log(WLR_ERROR, "file-watcher: wl_event_loop_add_fd failed; watching disabled");
        return false;
    }
    return true;
}

void FileWatcher::stop_if_idle() noexcept {
    if (!entries_.empty()) {
        return;
    }
    if (source_ != nullptr) {
        wl_event_source_remove(source_);
        source_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_); // drops all inotify watches
        fd_ = -1;
    }
    wd_dirs_.clear();
}

void FileWatcher::arm_dir(const std::string& dir) {
    if (fd_ < 0) {
        return;
    }
    // inotify_add_watch on an already-watched path returns the SAME wd, so this
    // is idempotent and auto re-arms after a watched file is replaced (we watch
    // the directory, not the inode).
    const int wd = inotify_add_watch(fd_, dir.c_str(),
                                     IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (wd >= 0) {
        wd_dirs_[wd] = dir;
    }
}

void FileWatcher::rearm_all_dirs() {
    for (const auto& [token, e] : entries_) {
        arm_dir(e.dir);
    }
}

auto FileWatcher::add(const std::string& path, std::function<void()> on_change, ExtensionId who)
    -> FileWatch {
    if (!ensure_started()) {
        return FileWatch{}; // no loop / inotify unavailable: inert handle
    }
    std::filesystem::path p(path);
    std::string dir = p.parent_path().string();
    if (dir.empty()) {
        dir = "."; // a bare filename watches the cwd
    }
    std::string base = p.filename().string();
    if (base.empty()) {
        return FileWatch{};
    }

    const Token token = ++next_token_;
    entries_.emplace(token, Entry{std::move(dir), std::move(base), std::move(on_change), who});
    arm_dir(entries_.at(token).dir);
    return FileWatch(this, token);
}

auto FileWatcher::add_dir(const std::string& dir, std::function<void()> on_change, ExtensionId who)
    -> FileWatch {
    if (!ensure_started()) {
        return FileWatch{};
    }
    std::string d = dir.empty() ? std::string(".") : dir;
    const Token token = ++next_token_;
    // Empty basename => match ANY file change in `d`.
    entries_.emplace(token, Entry{std::move(d), std::string{}, std::move(on_change), who});
    arm_dir(entries_.at(token).dir);
    return FileWatch(this, token);
}

void FileWatcher::remove_watch(Token token) noexcept {
    auto it = entries_.find(token);
    if (it == entries_.end()) {
        return;
    }
    entries_.erase(it);
    // Re-derive the inotify dir watches from the surviving entries: drop watches
    // for directories no longer referenced. Cheapest correct approach — clear
    // all wd watches and re-arm the dirs still in use. (Watch counts are tiny:
    // unbox.toml + a handful of asset dirs.)
    if (fd_ >= 0) {
        for (const auto& [wd, dir] : wd_dirs_) {
            inotify_rm_watch(fd_, wd);
        }
        wd_dirs_.clear();
        rearm_all_dirs();
    }
    stop_if_idle();
}

void FileWatcher::on_readable() {
    if (fd_ < 0) {
        return;
    }
    // Drain ALL queued events (one readable notification may carry many; the fd
    // is non-blocking). Collect the set of (dir, basename) pairs that changed,
    // then fire each matching watch's callback AT MOST ONCE this drain
    // (coalesced: one save = one callback even though it emits several events).
    alignas(struct inotify_event) char buf[4096];
    std::vector<std::pair<std::string, std::string>> changed; // (dir, name)
    for (;;) {
        const ssize_t n = read(fd_, buf, sizeof(buf));
        if (n <= 0) {
            break; // EAGAIN (drained) or closed
        }
        std::size_t off = 0;
        while (off + sizeof(struct inotify_event) <= static_cast<std::size_t>(n)) {
            auto* ev = reinterpret_cast<struct inotify_event*>(buf + off);
            auto wd_it = wd_dirs_.find(ev->wd);
            if (wd_it != wd_dirs_.end() && ev->len > 0) {
                changed.emplace_back(wd_it->second, std::string(ev->name));
            }
            off += sizeof(struct inotify_event) + ev->len;
        }
    }
    if (changed.empty()) {
        return;
    }

    // Find the tokens whose (dir, basename) matched at least one event. Snapshot
    // them so a callback that destroys its own (or another) watch mid-fire can't
    // invalidate the iteration.
    std::vector<Token> to_fire;
    for (const auto& [token, e] : entries_) {
        for (const auto& [dir, name] : changed) {
            // A file-specific entry matches its basename; a directory entry
            // (empty basename) matches ANY file change in its dir.
            if (e.dir == dir && (e.basename.empty() || e.basename == name)) {
                to_fire.push_back(token);
                break;
            }
        }
    }
    for (const Token token : to_fire) {
        auto it = entries_.find(token);
        if (it == entries_.end()) {
            continue; // removed by an earlier callback this drain
        }
        // Copy what we need before invoking: the callback may remove this watch.
        std::function<void()> cb = it->second.on_change;
        const ExtensionId who = it->second.who;
        if (!cb) {
            continue;
        }
        try {
            cb();
        } catch (...) {
            // Same isolation boundary as a throwing hook/getter: disable the
            // owning extension, never take down the loop/session.
            if (disable_) {
                disable_(who);
            }
        }
    }
}

} // namespace unbox::kernel
