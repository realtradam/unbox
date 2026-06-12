#include "demo_ui.hpp"

#include <unbox/ext-layer-shell/ext_layer_shell.hpp>
#include <unbox/ext-xdg-shell/ext_xdg_shell.hpp>
#include <unbox/kernel/kernel.hpp>
#include <unbox/kernel/server.hpp>

#include <cstdio>
#include <exception>
#include <string_view>

namespace {

void print_usage(const char* argv0) {
    std::printf("usage: %s [-s <startup command>] [--ui-demo]\n", argv0);
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    unbox::kernel::Server::Options options;
    bool ui_demo = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-s" && i + 1 < argc) {
            options.startup_cmd = argv[++i];
        } else if (arg == "--ui-demo") {
            // Slice-5 acceptance demo (temporary until slice 6's real UI
            // extensions): a ui surface via the public substrate contract.
            ui_demo = true;
        } else {
            print_usage(argv[0]);
            return arg == "-h" ? 0 : 1;
        }
    }

    try {
        auto server = unbox::kernel::Server::create(std::move(options));

        // The composition root: the ONLY place that names every extension.
        // install() transfers ownership; run() activates in dependency order.
        server->install(unbox::ext_xdg_shell::create());
        server->install(unbox::ext_layer_shell::create());
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
