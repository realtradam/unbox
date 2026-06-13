#include <unbox/ext-keybindings/ext_keybindings.hpp>

#include "config.hpp"
#include "focus_ring.hpp"
#include "policy.hpp"

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
          matcher_(load_bindings(config_path_)) {}

    auto manifest() const -> const kernel::Manifest& override { return manifest_; }

    void activate(Host& host) override {
        host_ = &host;

        // The ONLY fatal: a missing ext-xdg-shell Service (our focus ring + the
        // window-targeting actions are meaningless without it; depends_on
        // guarantees it activated first, so absence is a broken core session).
        shell_ = host.service<ext_xdg_shell::Service>();
        if (shell_ == nullptr) {
            throw std::runtime_error(
                "ext-keybindings: ext-xdg-shell Service unavailable (depends_on "
                "\"xdg-shell\" not satisfied)");
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
    }

private:
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
        .depends_on = {"xdg-shell"},
    };

    std::optional<std::string> config_path_;

    // Decision cores (constructed before any wiring; matcher_ owns the parsed
    // bindings). Declared before the subscriptions so they outlive callbacks
    // that capture `this` (members tear down in reverse declaration order:
    // subscriptions drop first, then the cores).
    policy::Matcher matcher_;
    policy::FocusRing<Toplevel*> ring_;

    Host* host_ = nullptr;
    ext_xdg_shell::Service* shell_ = nullptr; // borrow; fetched in activate()

    // RAII subscriptions — destruction unsubscribes (listener-lifetime). Last
    // members so they release FIRST at teardown, before the cores they touch.
    kernel::Subscription key_filter_;
    kernel::Subscription mapped_;
    kernel::Subscription unmapped_;
    kernel::Subscription focused_;
};

} // namespace

auto create(std::optional<std::string> config_path)
    -> std::unique_ptr<kernel::Extension> {
    return std::make_unique<KeybindingsExt>(std::move(config_path));
}

} // namespace unbox::ext_keybindings
