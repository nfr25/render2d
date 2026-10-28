// SPDX-License-Identifier: AGPL-3.0-or-later
// 2D affine transform, SVG convention (same numbers as cairo_matrix_t):
//
//     x' = a*x + c*y + e
//     y' = b*x + d*y + f
//
// i.e. the 3x3 matrix   | a c e |
//                       | b d f |
//                       | 0 0 1 |
//
// See notions §5 (matrix form) and §6 (inverse).
#pragma once

#include <optional>

#include "core/point.hpp"

namespace r2d {

struct Affine {
    double a = 1.0, b = 0.0;
    double c = 0.0, d = 1.0;
    double e = 0.0, f = 0.0;

    // --- factories ---------------------------------------------------------
    static Affine identity() { return {}; }
    static Affine translation(double tx, double ty) { return {1.0, 0.0, 0.0, 1.0, tx, ty}; }
    static Affine scaling(double sx, double sy)     { return {sx, 0.0, 0.0, sy, 0.0, 0.0}; }

    // --- use ---------------------------------------------------------------
    Point apply(Point p) const {
        return {a * p.x + c * p.y + e,
                b * p.x + d * p.y + f};
    }

    // Area ratio, and sign: negative means a mirror (our Y flip). notions §6.
    double determinant() const { return a * d - b * c; }

    // Empty when the transform is not invertible (determinant 0 or not finite).
    std::optional<Affine> inverse() const;
};

// Composition: (m * n).apply(p) == m.apply(n.apply(p)).
// Read right to left: n is applied first, then m. notions §5.
Affine operator*(const Affine& m, const Affine& n);

} // namespace r2d
