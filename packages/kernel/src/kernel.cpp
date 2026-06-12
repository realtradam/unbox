#include <unbox/kernel/kernel.hpp>
#include <unbox/kernel/wlr.hpp>

#include <RmlUi/Core/Core.h>

namespace unbox::kernel {

auto wlroots_version() -> std::string {
    return WLR_VERSION_STR;
}

auto rmlui_version() -> std::string {
    return Rml::GetVersion();
}

auto link_probe() -> bool {
    wlr_log_init(WLR_ERROR, nullptr);
    wl_display* display = wl_display_create();
    if (display == nullptr) {
        return false;
    }
    wl_display_destroy(display);
    return true;
}

} // namespace unbox::kernel
