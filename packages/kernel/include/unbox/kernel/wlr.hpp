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
// wlr-layer-shell (and its generated protocol header) name a struct field /
// request argument `namespace` — a valid C identifier but a C++ KEYWORD, which
// `extern "C"` does NOT exempt (it changes linkage, not lexing). Rename it to
// `_namespace` across the wlr includes, same scoped-macro discipline as
// `static` above (the Hyprland-proven fix). CONSEQUENCE that leaks through this
// public wrapper: code reaching wlr_layer_surface_v1::namespace must spell it
// `->_namespace`. Re-audit for further C++-keyword identifiers when adding
// protocol/wlr headers (only `namespace` collides in the current set).
#define namespace _namespace
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
// Slice-3 spike (RMLUi -> wlr_scene bridge): EGL/dmabuf, the GLES2 renderer's
// EGL accessors, buffer (dmabuf + data-ptr access for the shm fallback),
// swapchain, and DRM format sets. Re-audited the static-blanking gotcha for
// these: all are plain declarations; egl.h pulls in <EGL/egl*.h> which have
// no `static` tokens outside comments (verified). No header-inline function
// with a function-local `static` is introduced.
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
// Producer-side interface for the spike's custom data-ptr wlr_buffer (Plan B).
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
// wlr-layer-shell for ext-layer-shell. This header #includes the generated
// "wlr-layer-shell-unstable-v1-protocol.h" — produced by the wayland-scanner
// custom_target in packages/kernel/meson.build from the vendored
// protocol/wlr-layer-shell-unstable-v1.xml and propagated (include path +
// build order) through kernel_dep. Static-blanking re-audit: neither this
// wlroots header nor the generated protocol header contains a `static` storage
// keyword on a header-inline function with a function-local static (the
// generated header has only extern interface declarations; no array-param
// `[static N]` either), so the surrounding `#define static` is inert across
// both. The scene helper wlr_scene_layer_surface_v1 lives in wlr_scene.h
// (included below) — no second include needed.
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/version.h>
#undef namespace
#undef static
}
