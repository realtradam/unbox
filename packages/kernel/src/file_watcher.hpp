#pragma once

#include <unbox/kernel/hooks.hpp> // ExtensionId
#include <unbox/kernel/watch.hpp>
#include <unbox/kernel/wlr.hpp> // wl_event_loop / wl_event_source

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// The kernel's ONE inotify-on-the-wl_event_loop file watcher, shared by every
// consumer: Host::watch_file (config + any extension) AND the ui substrate's
// (UNBOX_DEV-gated) asset hot-reload. There is exactly one inotify instance per
// session; multiple watched paths multiplex over it.
//
// Editor-save safe: editors save by writing a temp file + renaming over the
// target, so the inode changes and IN_MODIFY on it is unreliable. We watch the
// containing DIRECTORY for IN_CLOSE_WRITE / IN_MOVED_TO / IN_CREATE and match
// the basename — this also fires when a not-yet-existing file is first created.
//
// Coalesced: a single readable notification is drained fully and each affected
// watch's callback fires AT MOST ONCE per drain (one save = one callback).
//
// Error-isolated: a throwing callback is caught at the boundary and the owning
// extension is disabled via the injected sink (same contract as hooks/getters),
// never the session.
//
// Lazy: the inotify fd + wl_event_loop source are created on the FIRST add()
// (whichever consumer is first) and torn down when the watcher is destroyed
// (before the loop/display dies) — kept open for the session while ≥1 watch
// lives; closed when the last watch is removed (re-created on the next add).
//
// Single wl_event_loop thread throughout; no internal locking.

namespace unbox::kernel {

class FileWatcher final : public detail::WatchRegistry {
public:
    using Token = detail::WatchRegistry::Token;

    // `loop` is the kernel's wl_event_loop (may be null on a backend without
    // one — then add() degrades to a no-op handle). `disable` disables the
    // owning extension when its callback throws (injected by the kernel, same
    // as the bus/substrate isolation sink).
    FileWatcher(wl_event_loop* loop, std::function<void(ExtensionId)> disable);
    ~FileWatcher() override;
    FileWatcher(const FileWatcher&) = delete;
    auto operator=(const FileWatcher&) -> FileWatcher& = delete;

    // Watch `path` (resolved by the caller to an absolute/usable path) for
    // content changes; fire `on_change` (coalesced, event-loop thread,
    // error-isolated to `who`). Returns a FileWatch RAII handle (inactive if
    // the watcher could not be set up). The handle removes the watch on destroy.
    [[nodiscard]] auto add(const std::string& path, std::function<void()> on_change,
                           ExtensionId who) -> FileWatch;

    // Watch the DIRECTORY `dir` for a change to ANY file within it (not a single
    // basename). Used by the ui-substrate's asset hot-reload: a document and its
    // `<link>`ed RCSS/assets live in the same dir, and editing ANY of them must
    // reload — without parsing the document's link set. Same coalescing / error
    // isolation / RAII as add(). `dir` should be an absolute directory path.
    [[nodiscard]] auto add_dir(const std::string& dir, std::function<void()> on_change,
                               ExtensionId who) -> FileWatch;

    // detail::WatchRegistry — stop the watch with this token (FileWatch dtor).
    void remove_watch(Token token) noexcept override;

private:
    struct Entry {
        std::string dir;       // watched directory (absolute)
        std::string basename;  // file within `dir` to match; EMPTY = any file
        std::function<void()> on_change;
        ExtensionId who{};
    };

    bool ensure_started();             // lazy inotify_init + wl_event_loop_add_fd
    void stop_if_idle() noexcept;      // close fd + source when no entries remain
    void arm_dir(const std::string& dir);   // (re)add the inotify dir watch
    void rearm_all_dirs();             // re-add every distinct dir's watch
    void on_readable();                // drain inotify, coalesce, fire callbacks

    static auto dispatch(int fd, std::uint32_t mask, void* data) -> int;

    wl_event_loop* loop_ = nullptr;
    std::function<void(ExtensionId)> disable_;
    int fd_ = -1;
    wl_event_source* source_ = nullptr;

    Token next_token_ = 0;
    std::unordered_map<Token, Entry> entries_;     // token -> watch
    std::unordered_map<int, std::string> wd_dirs_; // inotify wd -> directory
};

} // namespace unbox::kernel
