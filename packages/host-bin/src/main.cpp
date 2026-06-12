#include <unbox/kernel/kernel.hpp>

#include <cstdio>

auto main() -> int {
    const bool probe_ok = unbox::kernel::link_probe();

    std::printf("unbox 0.0.1 — kernel skeleton (slice 1)\n");
    std::printf("  wlroots    %s\n", unbox::kernel::wlroots_version().c_str());
    std::printf("  RmlUi      %s\n", unbox::kernel::rmlui_version().c_str());
    std::printf("  link probe %s\n", probe_ok ? "ok" : "FAILED");

    return probe_ok ? 0 : 1;
}
