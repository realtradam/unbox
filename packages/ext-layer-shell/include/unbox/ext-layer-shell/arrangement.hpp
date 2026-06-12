#pragma once

#include <cstdint>

// Arrangement math — the PURE CORE of ext-layer-shell. Zero wlroots/GL/RMLUi
// types: a self-contained model of the wlr-layer-shell usable-area bookkeeping
// the compositor must keep as anchored, exclusive-zone surfaces are arranged on
// an output. Input -> output only; doctest-covered hard. wlroots' own
// wlr_scene_layer_surface_v1_configure performs the actual scene positioning
// and mutates a usable_area box identically; this model is the independent,
// testable mirror we keep for everything DOWNSTREAM of the scene helper (the
// per-output usable area tiling, slice 7, will consume).
//
// Coordinate convention matches wlr_box: x/y are the box's top-left in output-
// local pixels, growing right/down. All math is integer (pixel) arithmetic.
//
// No allocation, no threads, no I/O. Pure value types.

namespace unbox::ext_layer_shell {

// An axis-aligned integer pixel box (mirrors wlr_box, but wlroots-free so this
// header stays a pure contract). width/height are >= 0 for a valid box.
struct Box {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    friend constexpr auto operator==(const Box&, const Box&) -> bool = default;
};

// Anchor bitfield — the wlr-layer-shell v1 `anchor` enum values verbatim
// (top=1, bottom=2, left=4, right=8), combinable. A surface anchored to two
// opposite edges spans that axis; anchored to all four it fills the output
// (minus margins). Kept as named constants so the pure core never needs the
// generated protocol header.
namespace anchor {
inline constexpr std::uint32_t top = 1;
inline constexpr std::uint32_t bottom = 2;
inline constexpr std::uint32_t left = 4;
inline constexpr std::uint32_t right = 8;
} // namespace anchor

// Which single edge a positive exclusive zone reserves space on. `none` means
// the surface reserves nothing (its exclusive zone is non-positive, or its
// anchoring makes a positive zone meaningless per the protocol — see
// exclusive_edge() below).
enum class Edge : std::uint8_t { none, top, bottom, left, right };

// The subset of a layer surface's committed state the arrangement math needs.
// `exclusive_edge` is the protocol's optional explicit override (v5): when
// Edge::none the edge is deduced from `anchor`; otherwise it forces the edge a
// corner-anchored surface reserves on. Margins are per-edge insets from the
// anchored edges.
struct SurfaceState {
    std::uint32_t anchor = 0;           // OR of anchor::* bits
    std::int32_t exclusive_zone = 0;    // <0 = "stretch over"; 0 = avoid; >0 = reserve
    std::int32_t margin_top = 0;
    std::int32_t margin_right = 0;
    std::int32_t margin_bottom = 0;
    std::int32_t margin_left = 0;
    Edge exclusive_edge = Edge::none;   // explicit override; none = deduce
};

// The edge a positive exclusive zone is applied to, or Edge::none if the zone
// reserves nothing. Mirrors wlr_layer_surface_v1_get_exclusive_edge():
//
//   * a non-positive exclusive_zone reserves nothing;
//   * an explicit exclusive_edge wins if it is one of the anchored edges;
//   * otherwise the edge is deducible only when the surface is anchored to
//     exactly one edge, OR to one edge plus its two perpendicular edges (a
//     full-width/height strip). Anchoring to a bare corner, to two parallel
//     edges, or to all four edges makes a positive zone meaningless -> none.
[[nodiscard]] constexpr auto exclusive_edge(const SurfaceState& s) -> Edge {
    if (s.exclusive_zone <= 0) {
        return Edge::none;
    }
    const bool t = (s.anchor & anchor::top) != 0;
    const bool b = (s.anchor & anchor::bottom) != 0;
    const bool l = (s.anchor & anchor::left) != 0;
    const bool r = (s.anchor & anchor::right) != 0;

    // Candidate edge from anchoring: a strip is anchored to one edge and
    // (optionally) both of the perpendicular edges, but NOT to the opposite
    // edge. Anchoring to all four, or to two parallel edges, yields no edge.
    Edge deduced = Edge::none;
    if (t && !b && (l == r)) {
        deduced = Edge::top;
    } else if (b && !t && (l == r)) {
        deduced = Edge::bottom;
    } else if (l && !r && (t == b)) {
        deduced = Edge::left;
    } else if (r && !l && (t == b)) {
        deduced = Edge::right;
    }

    if (s.exclusive_edge != Edge::none) {
        // Honor the explicit override only if it is an edge the surface is
        // actually anchored to (the protocol raises invalid_exclusive_edge
        // otherwise; wlroots clamps — we treat a non-anchored override as a
        // no-op deduction so a misbehaving client cannot warp the usable area).
        const bool anchored =
            (s.exclusive_edge == Edge::top && t) ||
            (s.exclusive_edge == Edge::bottom && b) ||
            (s.exclusive_edge == Edge::left && l) ||
            (s.exclusive_edge == Edge::right && r);
        // Still require the override to be on a strip (deduced != none) or a
        // corner where the override disambiguates a single anchored edge.
        if (anchored) {
            return s.exclusive_edge;
        }
        return Edge::none;
    }
    return deduced;
}

// Reduce `usable` by the space ONE surface reserves, returning the smaller box
// the NEXT lower surface in the same arrangement pass may use. Mirrors the
// usable_area mutation wlr_scene_layer_surface_v1_configure performs:
//
//   * a non-reserving surface (exclusive_edge() == none) leaves `usable` as-is;
//   * a reserving surface shrinks `usable` on its edge by
//     (exclusive_zone + the margin on that edge), clamped so width/height never
//     go negative.
//
// Apply this in protocol z-order (overlay first within a pass is irrelevant to
// the area; what matters is each surface sees the area left by those processed
// before it — drive the surfaces in a stable order and the result is the
// remaining usable area for non-exclusive content and for tiling).
[[nodiscard]] constexpr auto apply_exclusive(Box usable, const SurfaceState& s) -> Box {
    const Edge edge = exclusive_edge(s);
    if (edge == Edge::none) {
        return usable;
    }
    switch (edge) {
    case Edge::top: {
        const std::int32_t d = s.exclusive_zone + s.margin_top;
        usable.y += d;
        usable.height -= d;
        break;
    }
    case Edge::bottom: {
        const std::int32_t d = s.exclusive_zone + s.margin_bottom;
        usable.height -= d;
        break;
    }
    case Edge::left: {
        const std::int32_t d = s.exclusive_zone + s.margin_left;
        usable.x += d;
        usable.width -= d;
        break;
    }
    case Edge::right: {
        const std::int32_t d = s.exclusive_zone + s.margin_right;
        usable.width -= d;
        break;
    }
    case Edge::none:
        break;
    }
    if (usable.width < 0) {
        usable.width = 0;
    }
    if (usable.height < 0) {
        usable.height = 0;
    }
    return usable;
}

} // namespace unbox::ext_layer_shell
