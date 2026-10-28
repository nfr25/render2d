// SPDX-License-Identifier: AGPL-3.0-or-later
#include "render/labels.hpp"

#include <cmath>
#include <cstdio>

namespace r2d {

std::string labelText(const Value& v) {
    if (const auto* s = std::get_if<std::string>(&v)) return *s;
    if (const auto* i = std::get_if<long long>(&v))   return std::to_string(*i);
    if (const auto* d = std::get_if<double>(&v)) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.15g", *d);   // 120 or 120.5, never 120.000000
        return buf;
    }
    return "";
}

double readableAngle(double a) {
    a = std::fmod(a, 360.0);
    if (a < 0.0) a += 360.0;
    if (a > 90.0 && a <= 270.0) a -= 180.0;   // turn it the right way up
    if (a < 0.0) a += 360.0;
    return a;
}

std::vector<Point> labelPoints(const Geometry& g) {
    std::vector<Point> out;
    if (g.empty()) return out;
    switch (g.type) {
        case GeomType::Point:
            out = g.points;
            break;
        case GeomType::Line: {
            const Part& part = g.parts.front();
            const std::size_t mid = part.start + (part.count - 1) / 2;
            const Point a = g.points[mid];
            const Point b = part.count > 1 ? g.points[mid + 1] : a;
            out.push_back({(a.x + b.x) / 2.0, (a.y + b.y) / 2.0});
            break;
        }
        case GeomType::Polygon: {
            const Envelope e = g.envelope();
            out.push_back({(e.xmin + e.xmax) / 2.0, (e.ymin + e.ymax) / 2.0});
            break;
        }
    }
    return out;
}

Affine labelPlacement(const Label& l, const TextExtents& ink) {
    // 1. In text space, move the anchor of the ink box to (0, 0).
    //    anchorY counts upwards from the bottom of the box (SLD), y goes down.
    const double ax = ink.xBearing + l.anchorX * ink.width;
    const double ay = ink.yBearing + (1.0 - l.anchorY) * ink.height;
    const Affine anchor = Affine::translation(-ax, -ay);
    // 2. Rotate, clockwise on the image (y down: a positive angle turns clockwise). §36.
    const double t = l.rotation * 3.14159265358979323846 / 180.0;
    const Affine rotate{std::cos(t), std::sin(t), -std::sin(t), std::cos(t), 0.0, 0.0};
    // 3. Put it on the point, displaced (SLD's y goes up, the image's down).
    const Affine place = Affine::translation(l.at.x + l.dx, l.at.y - l.dy);
    return place * rotate * anchor;
}

std::size_t drawLabels(Canvas& cv, const std::vector<Label>& labels) {
    std::size_t drawn = 0;
    for (const Label& l : labels) {
        if (l.text.empty()) continue;
        const TextExtents ink = cv.measureText(l.text, l.font);
        cv.addText(l.text, l.font, labelPlacement(l, ink));
        if (l.halo && l.halo->radius > 0.0) {
            // §35: stroke the glyph outlines (half of the width falls outside),
            // then fill them on top. Round joins: no spikes on the 7.
            cv.stroke(StrokeStyle{.color = l.halo->color, .width = 2.0 * l.halo->radius,
                                  .cap = LineCap::Round, .join = LineJoin::Round},
                      PathMode::Keep);
        }
        cv.fill(FillStyle{.color = l.fill, .rule = FillRule::NonZero}, PathMode::Consume);
        ++drawn;
    }
    return drawn;
}

} // namespace r2d
