# wlroots-include
Never `#include <wlr/...>` or `<wayland-server*.h>` directly. Include
`unbox/kernel/wlr.hpp` (the kernel's extern-"C" wrapper) instead. wlroots
headers are C and break in C++ without the wrapper's guards, and the version
pin (0.20, `WLR_USE_UNSTABLE`) must live in exactly ONE place.
