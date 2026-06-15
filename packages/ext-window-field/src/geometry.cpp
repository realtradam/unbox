#include "geometry.hpp"

#include <algorithm>

namespace unbox::ext_window_field::geom {

auto apply_drag(Box start, Handle handle, int dx, int dy, Limits lim) -> Box {
    Box r = start;
    switch (handle) {
    case Handle::move:
        r.x = start.x + dx;
        r.y = start.y + dy;
        break;
    case Handle::resize_br:
        // Right + bottom edges follow the pointer; top-left anchored.
        r.w = std::max(lim.min_w, start.w + dx);
        r.h = std::max(lim.min_h, start.h + dy);
        break;
    case Handle::resize_bl:
        // Left + bottom edges follow; the RIGHT edge (start.x + start.w) and the
        // top edge stay anchored. Compute the new width first (clamped), then
        // place x so the right edge does not move.
        r.w = std::max(lim.min_w, start.w - dx);
        r.x = start.x + start.w - r.w;
        r.h = std::max(lim.min_h, start.h + dy);
        break;
    }
    return r;
}

auto clamp_to_field(Box b, int field_w, int field_h) -> Box {
    if (field_w > 0) {
        // Largest x that keeps the right edge inside; pin to 0 if the window is
        // wider than the field. Then clamp the origin into [0, max_x].
        const int max_x = std::max(0, field_w - b.w);
        b.x = std::clamp(b.x, 0, max_x);
    }
    if (field_h > 0) {
        const int max_y = std::max(0, field_h - b.h);
        b.y = std::clamp(b.y, 0, max_y);
    }
    return b;
}

} // namespace unbox::ext_window_field::geom
