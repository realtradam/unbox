// Single implementation TU for stb_image. Compiled as its own static library
// with warning_level=0 (see packages/kernel/meson.build) so stb's -Wall -Wextra
// warnings are completely isolated from the rest of the kernel build.
//
// stb_image returns pixels in R,G,B,A byte order (when requested with
// STBI_rgb_alpha / channels=4). The caller in rmlui_renderer_gl3.cpp uploads
// this RGBA8 data directly — do NOT apply the TGA's BGR swizzle here.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
