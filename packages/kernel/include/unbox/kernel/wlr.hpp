#pragma once

// The ONLY file in the project allowed to include wlroots / libwayland
// headers (.unbox/rules/wlroots-include.md). wlroots is C; this wrapper
// provides the extern-"C" guards C++ needs. WLR_USE_UNSTABLE is defined
// project-wide in the root meson.build; the version pin is wlroots 0.20.
//
// Grow this include list as kernel glue needs more types — never include
// <wlr/...> anywhere else, in any unit.

extern "C" {
#include <wayland-server-core.h>

// wlroots headers use C99 array-parameter syntax (`float color[static 4]`),
// which is invalid C++. Blanking `static` around the wlr includes is the
// proven workaround (Hyprland shipped years on it): `static inline` header
// helpers become plain `inline`, which C++ ODR-merges safely. Keep the
// #define scoped to EXACTLY these includes.
#define static
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/version.h>
#undef static
}
