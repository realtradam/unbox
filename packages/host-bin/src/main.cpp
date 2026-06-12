#include <unbox/kernel/kernel.hpp>
#include <unbox/kernel/server.hpp>

#include <cstdio>
#include <exception>
#include <string_view>

namespace {

void print_usage(const char* argv0) {
    std::printf("usage: %s [-s <startup command>]\n", argv0);
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    unbox::kernel::Server::Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-s" && i + 1 < argc) {
            options.startup_cmd = argv[++i];
        } else {
            print_usage(argv[0]);
            return arg == "-h" ? 0 : 1;
        }
    }

    try {
        auto server = unbox::kernel::Server::create(std::move(options));
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
