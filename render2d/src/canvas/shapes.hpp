// SPDX-License-Identifier: AGPL-3.0-or-later
// Shapes built only from the canvas vocabulary (lines and cubic Beziers).
// See notions §20 (circle) and §40 (well-known marks).
#pragma once

#include <cmath>
#include <initializer_list>

#include "canvas/canvas.hpp"

namespace r2d {

// k = 4/3 * (sqrt(2) - 1) ~= 0.55228: puts the middle of each quarter-curve
// exactly on the circle. Max radial error ~0.027 % of the radius.
inline const double kCircleKappa = 4.0 / 3.0 * (std::sqrt(2.0) - 1.0);

// Adds a closed circle to the current path, as four quarter Beziers.
// Image coordinates: y goes down, so the order is right, bottom, left, top.
inline void addCircle(Canvas& cv, Point c, double r) {
    const double k = kCircleKappa * r;
    cv.moveTo({c.x + r, c.y});
    cv.curveTo({c.x + r, c.y + k}, {c.x + k, c.y + r}, {c.x,     c.y + r});
    cv.curveTo({c.x - k, c.y + r}, {c.x - r, c.y + k}, {c.x - r, c.y    });
    cv.curveTo({c.x - r, c.y - k}, {c.x - k, c.y - r}, {c.x,     c.y - r});
    cv.curveTo({c.x + k, c.y - r}, {c.x + r, c.y - k}, {c.x + r, c.y    });
    cv.closePath();
}

// The well-known marks, in a frame from -1 to +1, y up, centred on (0, 0).
// `m` maps that frame to the image (scale, rotation, y flip, position). §40.
namespace marks {

inline void polygon(Canvas& cv, const Affine& m, std::initializer_list<Point> pts) {
    bool first = true;
    for (Point p : pts) {
        if (first) cv.moveTo(m.apply(p)); else cv.lineTo(m.apply(p));
        first = false;
    }
    cv.closePath();
}

inline void circle(Canvas& cv, const Affine& m) {   // four Beziers, mapped by m (§20)
    const double k = kCircleKappa;
    cv.moveTo(m.apply({1, 0}));
    cv.curveTo(m.apply({1, k}),  m.apply({k, 1}),  m.apply({0, 1}));
    cv.curveTo(m.apply({-k, 1}), m.apply({-1, k}), m.apply({-1, 0}));
    cv.curveTo(m.apply({-1, -k}), m.apply({-k, -1}), m.apply({0, -1}));
    cv.curveTo(m.apply({k, -1}), m.apply({1, -k}), m.apply({1, 0}));
    cv.closePath();
}

// Pentagram: 10 vertices every 36 degrees, radius 1 and 0.382 = sin 18 / sin 126.
inline void star(Canvas& cv, const Affine& m) {
    const double inner = std::sin(18.0 * 3.14159265358979323846 / 180.0) /
                         std::sin(126.0 * 3.14159265358979323846 / 180.0);
    for (int k = 0; k < 10; ++k) {
        const double a = (90.0 + 36.0 * k) * 3.14159265358979323846 / 180.0;
        const double r = k % 2 ? inner : 1.0;
        const Point p = m.apply({r * std::cos(a), r * std::sin(a)});
        if (k == 0) cv.moveTo(p); else cv.lineTo(p);
    }
    cv.closePath();
}

// A "+" of two bars 0.4 wide, as ONE outline (two rectangles would overlap). §40.
inline void cross(Canvas& cv, const Affine& m) {
    polygon(cv, m, {{-0.2, 1}, {0.2, 1}, {0.2, 0.2}, {1, 0.2}, {1, -0.2}, {0.2, -0.2},
                    {0.2, -1}, {-0.2, -1}, {-0.2, -0.2}, {-1, -0.2}, {-1, 0.2}, {-0.2, 0.2}});
}

inline void square(Canvas& cv, const Affine& m)   { polygon(cv, m, {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}); }
inline void triangle(Canvas& cv, const Affine& m) { polygon(cv, m, {{0, 1}, {-0.866, -0.5}, {0.866, -0.5}}); }

} // namespace marks

} // namespace r2d
