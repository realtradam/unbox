#include <unbox/ext-keybindings/ext_keybindings.hpp>

#include "config.hpp"
#include "focus_ring.hpp"
#include "policy.hpp"

#include <unbox/ext-stage-dock/ext_stage_dock.hpp>
#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/wlr.hpp>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

// ext-keybindings glue: the thin effectful edge. The decision cores live in
// src/policy.hpp (combo parser, matcher + tap SM), src/focus_ring.hpp (stable
// rotation), and src/config.* (toml loader) — all wlroots/GL-free and
// doctest-hard. THIS file only: loads the config file (effect), threads the
// kernel key_filter through the Matcher, mirrors ext-xdg-shell's toplevel
// lifecycle/focus events into the FocusRing, and performs the effects (fork/exec
// spawn, Toplevel::focus()/close(), wl_display_terminate).

namespace unbox::ext_keybindings {
namespace {

using kernel::Host;

// ext-xdg-shell's Toplevel* is the opaque token the focus ring rotates over. We
// NEVER deref it inside the ring; the glue derefs only a LIVE borrow (between its
// mapped and unmapped events) to call focus()/close().
using Toplevel = ext_xdg_shell::Toplevel;

// ---- spawn (effect): run a shell command without leaking zombies ------------
//
// Double-fork: the intermediate child forks the actual command then _exit()s
// immediately, so the grandchild is reparented to init (pid 1) and we never need
// a SIGCHLD handler — we waitpid() only the short-lived intermediate child. The
// command runs via `/bin/sh -c` (the brief's contract). Never blocks the event
// loop: the parent returns as soon as the intermediate child is reaped (which is
// immediate, since it only forks + exits).
void spawn_command(const std::string& command) {
    if (command.empty()) {
        return;
    }
    const pid_t intermediate = fork();
    if (intermediate < 0) {
        wlr_log(WLR_ERROR, "ext-keybindings: fork failed for command '%s'",
                command.c_str());
        return;
    }
    if (intermediate == 0) {
        // Intermediate child: detach into its own session, then fork the
        // grandchild that exec()s the command.
        setsid();
        const pid_t grandchild = fork();
        if (grandchild == 0) {
            execl("/bin/sh", "/bin/sh", "-c", command.c_str(), static_cast<char*>(nullptr));
            _exit(127); // exec failed
        }
        _exit(0); // intermediate exits immediately; grandchild -> init
    }
    // Parent: reap the intermediate child so it never lingers as a zombie. It
    // exits right away, so this does not block the event loop.
    int status = 0;
    waitpid(intermediate, &status, 0);
}

// ---- config load (effect): discover + read the file, parse, fall back -------
//
// Returns the bindings to install. Logs every warning. Discovery order when no
// explicit path: $XDG_CONFIG_HOME/unbox/unbox.toml, then ~/.config/unbox/
// unbox.toml. No readable file, a parse error, or a file with zero valid
// bindings -> the compiled-in DEFAULTS. Never throws.
auto read_file(const std::string& path, std::string& out) -> bool {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

auto discover_config_path(const std::optional<std::string>& explicit_path)
    -> std::optional<std::string> {
    if (explicit_path) {
        return explicit_path; // host-bin --config: use it verbatim
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::string(xdg) + "/unbox/unbox.toml";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::string(home) + "/.config/unbox/unbox.toml";
    }
    return std::nullopt;
}

auto load_bindings(const std::optional<std::string>& explicit_path)
    -> std::vector<policy::Binding> {
    const std::optional<std::string> path = discover_config_path(explicit_path);
    if (!path) {
        wlr_log(WLR_INFO, "ext-keybindings: no config path; using compiled defaults");
        return policy::default_bindings();
    }

    std::string text;
    if (!read_file(*path, text)) {
        if (explicit_path) {
            wlr_log(WLR_ERROR,
                    "ext-keybindings: --config '%s' not readable; using defaults",
                    path->c_str());
        } else {
            wlr_log(WLR_INFO, "ext-keybindings: no config at '%s'; using defaults",
                    path->c_str());
        }
        return policy::default_bindings();
    }

    config::LoadResult loaded = config::load_from_string(text);
    for (const std::string& w : loaded.warnings) {
        wlr_log(WLR_ERROR, "ext-keybindings: %s", w.c_str());
    }
    if (loaded.bindings.empty()) {
        wlr_log(WLR_INFO,
                "ext-keybindings: '%s' yielded no valid bindings; using defaults",
                path->c_str());
        return policy::default_bindings();
    }
    wlr_log(WLR_INFO, "ext-keybindings: loaded %zu binding(s) from '%s'",
            loaded.bindings.size(), path->c_str());
    return loaded.bindings;
}

// ---- The extension ----------------------------------------------------------

class KeybindingsExt final : public kernel::Extension {
public:
    explicit KeybindingsExt(std::optional<std::string> config_path)
        : config_path_(std::move(config_path)),
          effective_path_(discover_config_path(config_path_)),
          matcher_(load_bindings(config_path_)) {}

    auto manifest() const -> const kernel::Manifest& override { return manifest_; }

    void activate(Host& host) override {
        host_ = &host;

        // The ONLY fatals: missing ext-xdg-shell Service (actions that target
        // windows — focus, close — are meaningless without it), and missing
        // ext-stage-dock Service (dock-toggle-visible is a no-op without docks).
        // depends_on guarantees both activate first, so absence is a broken core.
        shell_ = host.service<ext_xdg_shell::Service>();
        if (shell_ == nullptr) {
            throw std::runtime_error(
                "ext-keybindings: ext-xdg-shell Service unavailable (depends_on "
                "\"xdg-shell\" not satisfied)");
        }
        dock_ = host.service<ext_stage_dock::Service>();
        if (dock_ == nullptr) {
            throw std::runtime_error(
                "ext-keybindings: ext-stage-dock Service unavailable (depends_on "
                "\"stage-dock\" not satisfied)");
        }

        // Input path: thread every key through the Matcher. Non-matching keys
        // pass through untouched; a matched chord is consumed, a tap fires its
        // action without consuming the (already-forwarded) modifier.
        key_filter_ = host.subscribe(
            host.key_filter(), [this](kernel::KeyEvent ev) {
                const auto out = matcher_.feed(ev.keysym, ev.modifiers, ev.pressed);
                if (out.fired != policy::Matcher::npos) {
                    const policy::Binding& b = matcher_.bindings()[out.fired];
                    run_action(b);
                    if (out.consume) {
                        ev.handled = true;
                    }
                }
                return ev;
            });

        // Focus ring: mirror ext-xdg-shell's toplevel lifecycle + focus into the
        // stable map-order ring. The Toplevel* borrow is valid from mapped until
        // unmapped (its contract), so storing it in the ring between those two
        // events is the supported pattern; we drop it on unmapped and never
        // deref it after.
        mapped_ = host.subscribe(
            shell_->on_toplevel_mapped(), [this](const ext_xdg_shell::ToplevelEvent& e) {
                ring_.add(e.toplevel);
                // A freshly mapped toplevel is the focused one (per ext-xdg-shell
                // map-focus); seed the cursor so the FIRST Alt+Tab steps off it.
                ring_.note_focused(e.toplevel);
            });
        unmapped_ = host.subscribe(
            shell_->on_toplevel_unmapped(), [this](const ext_xdg_shell::ToplevelEvent& e) {
                ring_.remove(e.toplevel);
            });
        focused_ = host.subscribe(
            shell_->on_toplevel_focused(), [this](const ext_xdg_shell::ToplevelEvent& e) {
                // Catches click/tap-to-focus and Alt+F1, so rotation always
                // continues from wherever focus ACTUALLY is.
                ring_.note_focused(e.toplevel);
            });

        // Config hot-reload: watch the EFFECTIVE config path so editing
        // unbox.toml re-applies keybindings live, with no restart. We watch even
        // if the file does not exist yet — the kernel fires on_change when a
        // not-yet-existing file is CREATED, so a user who later writes the config
        // gets it picked up. The FileWatch is a member (config_watch_) so the
        // watch lives exactly as long as this extension; on_change is coalesced
        // and error-isolated by the kernel. No path -> no watch (defaults stand).
        if (effective_path_) {
            config_watch_ = host.watch_file(*effective_path_, [this] { reload_config(); });
        }
    }

private:
    // Re-read + re-parse the EFFECTIVE config file and SWAP the live binding
    // table the key_filter link matches against. Runs on the event-loop thread
    // from the FileWatch callback. ROBUST by construction: an unreadable file, a
    // toml parse error, or a parse that yields zero usable bindings KEEPS the
    // currently-active bindings and logs exactly ONE warning — a half-saved /
    // broken-mid-edit file never drops the user's working keys, and nothing
    // throws out of this callback (toml::parse_error is caught inside the pure
    // config loader). On success the new table replaces matcher_ in place; the
    // key_filter lambda reads matcher_ through `this`, so the swap takes effect
    // immediately with NO re-subscribe.
    void reload_config() {
        if (!effective_path_) {
            return; // defensive: only registered when a path exists
        }

        std::string text;
        if (!read_file(*effective_path_, text)) {
            // File vanished or became unreadable mid-edit (e.g. an editor's
            // unlink+rename window). Keep the working keys; the watch stays
            // armed for the next write.
            wlr_log(WLR_ERROR,
                    "ext-keybindings: config '%s' not readable on reload; keeping current bindings",
                    effective_path_->c_str());
            return;
        }

        // Pure swap-or-keep-old decision over the CURRENTLY-LIVE table.
        config::ReloadDecision decision =
            config::reload_bindings(matcher_.bindings(), text);
        if (!decision.swapped) {
            // Parse error or zero usable bindings: keep current, ONE warning.
            wlr_log(WLR_ERROR,
                    "ext-keybindings: reload of '%s' failed (%zu issue(s)); keeping current bindings",
                    effective_path_->c_str(), decision.warnings.size());
            return;
        }

        // SUCCESS: swap the live matcher (and thus its binding table) in place.
        const std::size_t n = decision.bindings.size();
        matcher_ = policy::Matcher(std::move(decision.bindings));
        wlr_log(WLR_INFO, "ext-keybindings: config reloaded (%zu binding(s)) from '%s'",
                n, effective_path_->c_str());
    }

    void run_action(const policy::Binding& b) {
        switch (b.action) {
        case policy::Action::spawn:
            spawn_command(b.command);
            return;
        case policy::Action::focus_next:
            rotate(/*forward=*/true);
            return;
        case policy::Action::focus_prev:
            rotate(/*forward=*/false);
            return;
        case policy::Action::close_active:
            close_active();
            return;
        case policy::Action::quit:
            wl_display_terminate(host_->display());
            return;
        case policy::Action::dock_toggle_visible:
            dock_->toggle_visible();
            return;
        }
    }

    void rotate(bool forward) {
        // ring_.next()/prev() return `const Token*` (Token == Toplevel*), i.e.
        // a pointer to a stored, still-live Toplevel*; null means 0 windows.
        Toplevel* const* p = forward ? ring_.next() : ring_.prev();
        if (p == nullptr || *p == nullptr) {
            return; // 0 windows
        }
        Toplevel* next = *p;
        next->focus();
        // Set current ourselves rather than relying solely on the focused event
        // echoing back (brief). The on_toplevel_focused subscription still keeps
        // us in sync with external focus changes.
        ring_.set_current(next);
    }

    void close_active() {
        // current() returns the live cursor token (valid until its unmapped
        // event), or null if none. The stored token IS a mutable Toplevel*.
        Toplevel* const* cur = ring_.current();
        if (cur != nullptr && *cur != nullptr) {
            (*cur)->close();
        }
    }

    const kernel::Manifest manifest_{
        .id = "keybindings",
        .tier = kernel::Tier::core,
        .depends_on = {"xdg-shell", "stage-dock"},
    };

    std::optional<std::string> config_path_;
    // The EFFECTIVE config path (explicit --config, else the discovered XDG /
    // ~/.config path), resolved ONCE in the ctor. Drives both the initial load
    // and the hot-reload watch + re-read. nullopt only when no HOME/XDG exists.
    // Declared before matcher_ so it is initialized first (ctor init order).
    std::optional<std::string> effective_path_;

    // Decision cores (constructed before any wiring; matcher_ owns the parsed
    // bindings). matcher_ is the LIVE binding table the key_filter link reads
    // through `this`; reload_config() swaps it in place. Declared before the
    // subscriptions so they outlive callbacks that capture `this` (members tear
    // down in reverse declaration order: subscriptions drop first, then cores).
    policy::Matcher matcher_;
    policy::FocusRing<Toplevel*> ring_;

    Host* host_ = nullptr;
    ext_xdg_shell::Service* shell_ = nullptr;  // borrow; fetched in activate()
    ext_stage_dock::Service* dock_ = nullptr;  // borrow; fetched in activate()

    // RAII subscriptions + the file watch — destruction unsubscribes / stops the
    // watch (listener-lifetime). Last members so they release FIRST at teardown,
    // before the cores their callbacks touch (matcher_, ring_). config_watch_'s
    // on_change reads matcher_ + effective_path_, so it must die before them.
    kernel::Subscription key_filter_;
    kernel::Subscription mapped_;
    kernel::Subscription unmapped_;
    kernel::Subscription focused_;
    kernel::FileWatch config_watch_;
};

} // namespace

auto create(std::optional<std::string> config_path)
    -> std::unique_ptr<kernel::Extension> {
    return std::make_unique<KeybindingsExt>(std::move(config_path));
}

} // namespace unbox::ext_keybindings
