#include "demo_ui.hpp"

#include <unbox/ext-keybindings/ext_keybindings.hpp>
#include <unbox/ext-layer-shell/ext_layer_shell.hpp>
#include <unbox/ext-stage-dock/ext_stage_dock.hpp>
#include <unbox/ext-window-field/ext_window_field.hpp>
#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>
#include <unbox/kernel/kernel.hpp>
#include <unbox/kernel/server.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace {

void print_usage(const char* argv0) {
    std::printf("usage: %s [-s <startup command>] [--config <path>] [--ui-demo] "
                "[--rml-compositing]\n",
                argv0);
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    unbox::kernel::Server::Options options;
    bool ui_demo = false;
    // RML compositing (Phase 2): when on, install ext-window-field, which
    // composites toplevels as RCSS surface elements instead of wlr_scene nodes.
    // Default OFF so the proven wlr_scene path stays the default until RML
    // compositing is signed off on the real seat. Enable via --rml-compositing
    // or the UNBOX_RML_COMPOSITING env var (the flag is intentionally a runtime
    // toggle so the two compositing paths can be A/B'd on hardware).
    bool rml_compositing = std::getenv("UNBOX_RML_COMPOSITING") != nullptr;
    std::optional<std::string> config_path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-s" && i + 1 < argc) {
            options.startup_cmd = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            // Explicit unbox.toml for ext-keybindings; if omitted it discovers
            // the XDG path, then falls back to compiled-in defaults.
            config_path = argv[++i];
        } else if (arg == "--ui-demo") {
            // Slice-5 acceptance demo (temporary until the real UI extensions):
            // a ui surface via the public substrate contract.
            ui_demo = true;
        } else if (arg == "--rml-compositing") {
            rml_compositing = true;
        } else {
            print_usage(argv[0]);
            return arg == "-h" ? 0 : 1;
        }
    }

    try {
        auto server = unbox::kernel::Server::create(std::move(options));

        // The composition root: the ONLY place that names every extension.
        // install() transfers ownership; run() activates in dependency order
        // (ext-keybindings depends_on xdg-shell, resolved topologically).
        server->install(unbox::ext_xdg_shell::create());
        server->install(unbox::ext_layer_shell::create());
        server->install(unbox::ext_keybindings::create(config_path));
        // The stage dock: Super+M minimizes the focused window into a left-edge
        // dock of previews; tap a preview to restore. Standard tier, hidden until
        // it holds a minimized window (depends_on xdg-shell, topo-activated).
        server->install(unbox::ext_stage_dock::create());
        // RML compositing (Phase 2, opt-in): the window field composites toplevels
        // as RCSS surface elements. depends_on "xdg-shell" (topologically
        // activated). When off, toplevels keep compositing through wlr_scene.
        if (rml_compositing) {
            server->install(unbox::ext_window_field::create());
        }
        if (ui_demo) {
            server->install(unbox::host_bin::create_demo_ui());
        }

        std::printf("unbox 0.0.1 (wlroots %s, RmlUi %s) on WAYLAND_DISPLAY=%s\n",
                    unbox::kernel::wlroots_version().c_str(),
                    unbox::kernel::rmlui_version().c_str(), server->socket_name().c_str());
        server->run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unbox: fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
