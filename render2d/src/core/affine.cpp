// SPDX-License-Identifier: AGPL-3.0-or-later
#include "core/affine.hpp"

#include <cmath>

namespace r2d {

// Plain 3x3 product, keeping only the 6 meaningful entries
// (the last row 0 0 1 stays 0 0 1).
Affine operator*(const Affine& m, const Affine& n) {
    Affine r;
    r.a = m.a * n.a + m.c * n.b;
    r.b = m.b * n.a + m.d * n.b;
    r.c = m.a * n.c + m.c * n.d;
    r.d = m.b * n.c + m.d * n.d;
    r.e = m.a * n.e + m.c * n.f + m.e;
    r.f = m.b * n.e + m.d * n.f + m.f;
    return r;
}

// Inverse through the determinant of the 2x2 block. notions §6:
//   X = ( d*(x - e) - c*(y - f)) / det
//   Y = (-b*(x - e) + a*(y - f)) / det
// expanded into the six coefficients of the inverse transform.
std::optional<Affine> Affine::inverse() const {
    const double det = determinant();
    if (det == 0.0 || !std::isfinite(det)) {
        return std::nullopt;
    }
    Affine r;
    r.a =  d / det;
    r.b = -b / det;
    r.c = -c / det;
    r.d =  a / det;
    r.e = (c * f - d * e) / det;
    r.f = (b * e - a * f) / det;
    return r;
}

} // namespace r2d
