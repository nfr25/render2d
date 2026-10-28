// SPDX-License-Identifier: AGPL-3.0-or-later
// The renderer's own geometry: OGC Simple Features, flattened into two arrays.
// One Geometry = one feature. A Polygon with a courtyard has 2 parts, a
// MultiPolygon with an island has 2 parts too. See notions §15 and §16.
#pragma once

#include <cstddef>
#include <vector>

#include "core/envelope.hpp"
#include "core/point.hpp"

namespace r2d {

// Multi* types are folded into their simple type: parts carry the "multi".
enum class GeomType { Point, Line, Polygon };

// One ring (polygon), one line, or one point, inside Geometry::points.
struct Part {
    std::size_t start = 0;
    std::size_t count = 0;
};

struct Geometry {
    GeomType           type = GeomType::Point;
    std::vector<Point> points;
    std::vector<Part>  parts;

    // Keeps the allocated memory: one Geometry is reused for every feature.
    void clear() { points.clear(); parts.clear(); }

    bool empty() const { return points.empty(); }

    // Appends a part made of the points added since `start`.
    void closePart(std::size_t start) {
        if (points.size() > start) parts.push_back({start, points.size() - start});
    }

    Envelope envelope() const {
        Envelope e{};
        if (points.empty()) return e;
        e = {points[0].x, points[0].y, points[0].x, points[0].y};
        for (const Point& p : points) {
            if (p.x < e.xmin) e.xmin = p.x;
            if (p.x > e.xmax) e.xmax = p.x;
            if (p.y < e.ymin) e.ymin = p.y;
            if (p.y > e.ymax) e.ymax = p.y;
        }
        return e;
    }
};

} // namespace r2d
