#pragma once

#include <array>
#include <cmath>
#include <optional>

// SPIKE (rml-compositing, Phase 0) — PURE input-inversion core. NO wlroots / GL
// / RMLUi types, so it is doctest-able with nothing running (AGENTS.md: pure
// decision cores tested hard). Throwaway: proves the MATH that criterion 3
// stands on — translating a point picked on a 3D-transformed surface element
// back to surface-LOCAL coordinates, which then becomes a wl_seat notify.
//
// Why this exists separately from "RmlUi does the picking for us": RmlUi's
// Context::ProcessMouse*/ProcessTouch* DO the transform-aware hit-test and report
// the event's mouse_x/mouse_y already in element/surface-local space (the
// substrate's ctx_motion proves this — it feeds context coords relative to the
// surface origin and reads mouse_x/mouse_y straight back as surface-local px).
// The spike still owns the FORWARD projection: to TEST that round trip
// objectively without eyes, it must (a) place a surface-local point, (b) project
// it THROUGH the same 3D transform RCSS applies to find where it lands on the
// flat output (the "screen" point a finger would touch), then (c) confirm the
// inverse recovers the original surface-local point. If forward∘inverse is
// identity to sub-pixel tolerance through a perspective+rotateY, the geometry
// criterion 3 needs is sound; the live wiring (RmlUi pick -> wl_seat) is then a
// thin call proven at runtime in the GL spike.
//
// Everything is column-vector math with COLUMN-MAJOR 4x4 matrices, matching the
// convention RmlUi's Matrix4f uses for `transform` (so a matrix authored here
// maps 1:1 onto an RCSS transform when cross-checked). Single-thread; no state.

namespace unbox::kernel::spike {

// A column-major 4x4 matrix: m[col*4 + row]. v' = M * v.
struct Mat4 {
    std::array<double, 16> m{};

    static auto identity() -> Mat4 {
        Mat4 r;
        r.m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        return r;
    }

    auto at(int row, int col) const -> double { return m[static_cast<std::size_t>(col) * 4 + row]; }
    auto at(int row, int col) -> double& { return m[static_cast<std::size_t>(col) * 4 + row]; }
};

// Column-major multiply: returns A*B.
inline auto mul(const Mat4& a, const Mat4& b) -> Mat4 {
    Mat4 r;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) {
                s += a.at(row, k) * b.at(k, col);
            }
            r.at(row, col) = s;
        }
    }
    return r;
}

// A homogeneous 4-vector.
struct Vec4 {
    double x{}, y{}, z{}, w{};
};

inline auto apply(const Mat4& mtx, const Vec4& v) -> Vec4 {
    return Vec4{
        mtx.at(0, 0) * v.x + mtx.at(0, 1) * v.y + mtx.at(0, 2) * v.z + mtx.at(0, 3) * v.w,
        mtx.at(1, 0) * v.x + mtx.at(1, 1) * v.y + mtx.at(1, 2) * v.z + mtx.at(1, 3) * v.w,
        mtx.at(2, 0) * v.x + mtx.at(2, 1) * v.y + mtx.at(2, 2) * v.z + mtx.at(2, 3) * v.w,
        mtx.at(3, 0) * v.x + mtx.at(3, 1) * v.y + mtx.at(3, 2) * v.z + mtx.at(3, 3) * v.w,
    };
}

// ---- RCSS-equivalent transform builders (column-major) ----------------------

// CSS `perspective(d)`: m[3][2] = -1/d (column-major: at(3,2)). A point at
// model-z is foreshortened by w = 1 - z/d after the divide.
inline auto perspective(double d) -> Mat4 {
    Mat4 r = Mat4::identity();
    r.at(3, 2) = -1.0 / d;
    return r;
}

// CSS `rotateY(theta)` (radians). Right-handed about +Y.
inline auto rotate_y(double theta) -> Mat4 {
    Mat4 r = Mat4::identity();
    const double c = std::cos(theta);
    const double s = std::sin(theta);
    r.at(0, 0) = c;
    r.at(0, 2) = s;
    r.at(2, 0) = -s;
    r.at(2, 2) = c;
    return r;
}

// CSS `translate(tx,ty)` in the XY plane.
inline auto translate(double tx, double ty) -> Mat4 {
    Mat4 r = Mat4::identity();
    r.at(0, 3) = tx;
    r.at(1, 3) = ty;
    return r;
}

// ---- The transform RCSS actually applies around transform-origin -------------
//
// RCSS resolves `transform` about `transform-origin` (default 50% 50%): it
// translates the origin to (0,0), applies the listed functions, then translates
// back. This builds that full operator for a surface element of size w*h with
// the given origin, so the math matches what RmlUi computes for the element.
inline auto rcss_transform_about_origin(const Mat4& t, double origin_x, double origin_y) -> Mat4 {
    return mul(translate(origin_x, origin_y), mul(t, translate(-origin_x, -origin_y)));
}

// ---- Forward projection: surface-local (lx,ly) -> screen point ---------------
//
// Place a surface-local point on the z=0 plane, push it through the element
// transform, perform the perspective divide, and return the on-screen (sx,sy)
// where a finger/cursor would land. This is the point the GL spike feeds to
// RmlUi's ProcessMouse*/ProcessTouch*.
struct ScreenPoint {
    double x{}, y{};
};

inline auto project_to_screen(const Mat4& transform, double lx, double ly) -> ScreenPoint {
    const Vec4 clip = apply(transform, Vec4{lx, ly, 0.0, 1.0});
    const double inv_w = (std::abs(clip.w) < 1e-12) ? 0.0 : 1.0 / clip.w;
    return ScreenPoint{clip.x * inv_w, clip.y * inv_w};
}

// ---- Inverse: screen point -> surface-local (lx,ly) --------------------------
//
// Inverting the projection is a ray/plane intersection (the transform is not
// affine under perspective). We invert the 4x4 transform, take the screen point
// as a clip-space ray (two points at different homogeneous depths), transform
// both back to model space, and intersect the resulting model-space ray with
// the element's own z=0 plane. The intersection's (x,y) is the surface-local
// coordinate. Returns nullopt if the transform is singular or the ray is
// parallel to the plane (degenerate edge-on view).

// General 4x4 inverse (column-major). nullopt if |det| ~ 0.
inline auto invert(const Mat4& a) -> std::optional<Mat4> {
    const std::array<double, 16>& s = a.m;
    std::array<double, 16> inv{};

    inv[0] = s[5] * s[10] * s[15] - s[5] * s[11] * s[14] - s[9] * s[6] * s[15] +
             s[9] * s[7] * s[14] + s[13] * s[6] * s[11] - s[13] * s[7] * s[10];
    inv[4] = -s[4] * s[10] * s[15] + s[4] * s[11] * s[14] + s[8] * s[6] * s[15] -
             s[8] * s[7] * s[14] - s[12] * s[6] * s[11] + s[12] * s[7] * s[10];
    inv[8] = s[4] * s[9] * s[15] - s[4] * s[11] * s[13] - s[8] * s[5] * s[15] +
             s[8] * s[7] * s[13] + s[12] * s[5] * s[11] - s[12] * s[7] * s[9];
    inv[12] = -s[4] * s[9] * s[14] + s[4] * s[10] * s[13] + s[8] * s[5] * s[14] -
              s[8] * s[6] * s[13] - s[12] * s[5] * s[10] + s[12] * s[6] * s[9];
    inv[1] = -s[1] * s[10] * s[15] + s[1] * s[11] * s[14] + s[9] * s[2] * s[15] -
             s[9] * s[3] * s[14] - s[13] * s[2] * s[11] + s[13] * s[3] * s[10];
    inv[5] = s[0] * s[10] * s[15] - s[0] * s[11] * s[14] - s[8] * s[2] * s[15] +
             s[8] * s[3] * s[14] + s[12] * s[2] * s[11] - s[12] * s[3] * s[10];
    inv[9] = -s[0] * s[9] * s[15] + s[0] * s[11] * s[13] + s[8] * s[1] * s[15] -
             s[8] * s[3] * s[13] - s[12] * s[1] * s[11] + s[12] * s[3] * s[9];
    inv[13] = s[0] * s[9] * s[14] - s[0] * s[10] * s[13] - s[8] * s[1] * s[14] +
              s[8] * s[2] * s[13] + s[12] * s[1] * s[10] - s[12] * s[2] * s[9];
    inv[2] = s[1] * s[6] * s[15] - s[1] * s[7] * s[14] - s[5] * s[2] * s[15] +
             s[5] * s[3] * s[14] + s[13] * s[2] * s[7] - s[13] * s[3] * s[6];
    inv[6] = -s[0] * s[6] * s[15] + s[0] * s[7] * s[14] + s[4] * s[2] * s[15] -
             s[4] * s[3] * s[14] - s[12] * s[2] * s[7] + s[12] * s[3] * s[6];
    inv[10] = s[0] * s[5] * s[15] - s[0] * s[7] * s[13] - s[4] * s[1] * s[15] +
              s[4] * s[3] * s[13] + s[12] * s[1] * s[7] - s[12] * s[3] * s[5];
    inv[14] = -s[0] * s[5] * s[14] + s[0] * s[6] * s[13] + s[4] * s[1] * s[14] -
              s[4] * s[2] * s[13] - s[12] * s[1] * s[6] + s[12] * s[2] * s[5];
    inv[3] = -s[1] * s[6] * s[11] + s[1] * s[7] * s[10] + s[5] * s[2] * s[11] -
             s[5] * s[3] * s[10] - s[9] * s[2] * s[7] + s[9] * s[3] * s[6];
    inv[7] = s[0] * s[6] * s[11] - s[0] * s[7] * s[10] - s[4] * s[2] * s[11] +
             s[4] * s[3] * s[10] + s[8] * s[2] * s[7] - s[8] * s[3] * s[6];
    inv[11] = -s[0] * s[5] * s[11] + s[0] * s[7] * s[9] + s[4] * s[1] * s[11] -
              s[4] * s[3] * s[9] - s[8] * s[1] * s[7] + s[8] * s[3] * s[5];
    inv[15] = s[0] * s[5] * s[10] - s[0] * s[6] * s[9] - s[4] * s[1] * s[10] +
              s[4] * s[2] * s[9] + s[8] * s[1] * s[6] - s[8] * s[2] * s[5];

    double det = s[0] * inv[0] + s[1] * inv[4] + s[2] * inv[8] + s[3] * inv[12];
    if (std::abs(det) < 1e-12) {
        return std::nullopt;
    }
    det = 1.0 / det;
    Mat4 r;
    for (int i = 0; i < 16; ++i) {
        r.m[static_cast<std::size_t>(i)] = inv[static_cast<std::size_t>(i)] * det;
    }
    return r;
}

struct LocalPoint {
    double x{}, y{};
};

// Unproject a screen point through `transform` back onto the element's z=0
// plane. `transform` is the same forward operator used by project_to_screen
// (RCSS transform about origin). Returns the surface-local (lx,ly).
inline auto unproject_to_local(const Mat4& transform, double sx, double sy)
    -> std::optional<LocalPoint> {
    const std::optional<Mat4> inv = invert(transform);
    if (!inv) {
        return std::nullopt;
    }
    // Two clip-space points along the viewing ray at the screen pixel: clip-z
    // is free under an orthographic screen, so pick z=0 and z=1 (homogeneous
    // w=1) and map both back to model space, then intersect with model z=0.
    const Vec4 a = apply(*inv, Vec4{sx, sy, 0.0, 1.0});
    const Vec4 b = apply(*inv, Vec4{sx, sy, 1.0, 1.0});
    const auto dehom = [](const Vec4& v) -> Vec4 {
        const double iw = (std::abs(v.w) < 1e-12) ? 0.0 : 1.0 / v.w;
        return Vec4{v.x * iw, v.y * iw, v.z * iw, 1.0};
    };
    const Vec4 pa = dehom(a);
    const Vec4 pb = dehom(b);
    const double dz = pb.z - pa.z;
    if (std::abs(dz) < 1e-12) {
        return std::nullopt; // ray parallel to the element plane
    }
    const double t = (0.0 - pa.z) / dz; // param where the ray crosses z=0
    return LocalPoint{pa.x + (pb.x - pa.x) * t, pa.y + (pb.y - pa.y) * t};
}

} // namespace unbox::kernel::spike
