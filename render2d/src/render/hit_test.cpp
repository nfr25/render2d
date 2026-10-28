// SPDX-License-Identifier: AGPL-3.0-or-later
#include "render/hit_test.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace r2d {

double distanceToSegment(Point p, Point a, Point b) {
    const double abx = b.x - a.x, aby = b.y - a.y;
    const double len2 = abx * abx + aby * aby;
    // t = (AP . AB) / |AB|^2, kept in [0, 1]: the nearest point of the segment.
    double t = len2 > 0.0 ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p.x - (a.x + t * abx), p.y - (a.y + t * aby));
}

double distanceToParts(const Geometry& g, Point p, bool closed) {
    double best = std::numeric_limits<double>::infinity();
    for (const Part& part : g.parts) {
        const Point* pts = g.points.data() + part.start;
        for (std::size_t i = 0; i + 1 < part.count; ++i)
            best = std::min(best, distanceToSegment(p, pts[i], pts[i + 1]));
        if (closed && part.count > 2)   // the closing edge (often already there: first == last)
            best = std::min(best, distanceToSegment(p, pts[part.count - 1], pts[0]));
        if (part.count == 1)
            best = std::min(best, std::hypot(p.x - pts[0].x, p.y - pts[0].y));
    }
    return best;
}

double distanceToPoints(const Geometry& g, Point p) {
    double best = std::numeric_limits<double>::infinity();
    for (const Point& q : g.points) best = std::min(best, std::hypot(p.x - q.x, p.y - q.y));
    return best;
}

bool insideEvenOdd(const Geometry& g, Point p) {
    bool inside = false;
    for (const Part& part : g.parts) {
        const Point* pts = g.points.data() + part.start;
        const std::size_t n = part.count;
        // Each edge (a, b), the ring closed: does the half-line y = p.y, x > p.x cross it?
        for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
            const Point& a = pts[i];
            const Point& b = pts[j];
            // One end strictly above, the other not: a vertex exactly on the
            // half-line is counted once, not twice (O'Rourke's rule).
            if ((a.y > p.y) != (b.y > p.y)) {
                const double x = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
                if (x > p.x) inside = !inside;
            }
        }
    }
    return inside;
}

double orient(Point a, Point b, Point c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

namespace {

// c, known to be on the line ab, lies between a and b.
bool onSegment(Point a, Point b, Point c) {
    return std::min(a.x, b.x) <= c.x && c.x <= std::max(a.x, b.x) &&
           std::min(a.y, b.y) <= c.y && c.y <= std::max(a.y, b.y);
}

bool opposite(double u, double v) { return (u > 0.0 && v < 0.0) || (u < 0.0 && v > 0.0); }

// Calls edge(a, b) for every segment of the geometry; stops when it returns true.
// `closed`: each part is a ring, its last point joined to its first.
template <class F> bool anyEdge(const Geometry& g, bool closed, F edge) {
    for (const Part& part : g.parts) {
        const Point* pts = g.points.data() + part.start;
        for (std::size_t i = 0; i + 1 < part.count; ++i)
            if (edge(pts[i], pts[i + 1])) return true;
        if (closed && part.count > 2 && edge(pts[part.count - 1], pts[0])) return true;
    }
    return false;
}

bool overlaps(const Envelope& a, const Envelope& b) {
    return a.xmin <= b.xmax && b.xmin <= a.xmax && a.ymin <= b.ymax && b.ymin <= a.ymax;
}

} // namespace

bool segmentsCross(Point a, Point b, Point c, Point d) {
    const double o1 = orient(a, b, c), o2 = orient(a, b, d);
    const double o3 = orient(c, d, a), o4 = orient(c, d, b);
    // C and D on both sides of AB, and A and B on both sides of CD (§55).
    if (opposite(o1, o2) && opposite(o3, o4)) return true;
    // The limit cases: one end exactly on the other segment.
    return (o1 == 0.0 && onSegment(a, b, c)) || (o2 == 0.0 && onSegment(a, b, d)) ||
           (o3 == 0.0 && onSegment(c, d, a)) || (o4 == 0.0 && onSegment(c, d, b));
}

bool touchesArea(const Geometry& g, const Geometry& zone) {
    if (g.empty() || zone.empty() || !overlaps(g.envelope(), zone.envelope())) return false;

    // 1. A vertex in the zone (for a point: the only test).
    for (const Point& p : g.points)
        if (insideEvenOdd(zone, p)) return true;
    if (g.type == GeomType::Point) return false;

    // 2. An edge crossing an edge of the zone: a street through a small rectangle.
    const bool closed = g.type == GeomType::Polygon;
    const bool crossing = anyEdge(g, closed, [&](Point a, Point b) {
        return anyEdge(zone, true, [&](Point c, Point d) { return segmentsCross(a, b, c, d); });
    });
    if (crossing) return true;

    // 3. The zone inside the polygon: a small rectangle in a big parcel. Not
    //    in a courtyard: even-odd, as the fill (§54).
    return closed && insideEvenOdd(g, zone.points.front());
}

} // namespace r2d
