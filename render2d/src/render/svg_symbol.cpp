// SPDX-License-Identifier: AGPL-3.0-or-later
#include "render/svg_symbol.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"

#include "canvas/canvas.hpp"

namespace r2d {

namespace {

struct Freer { void operator()(NSVGimage* i) const { nsvgDelete(i); } };
struct Entry {
    std::unique_ptr<NSVGimage, Freer> image;   // null: could not be read
    std::string                       error;
};

// One parse per file for the whole program. editor2d draws in two threads
// (the render and the queries): a lock around the cache; the images, once
// read, are only read.
std::mutex                    g_mutex;
std::map<std::string, Entry>  g_cache;

const NSVGimage* load(const std::string& path, std::string& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(path);
    if (it == g_cache.end()) {
        Entry e;
        // "px" and 96 dpi: the units of a screen, as browsers read SVG.
        e.image.reset(nsvgParseFromFile(path.c_str(), "px", 96.0f));
        if (!e.image) e.error = "cannot read the SVG '" + path + "'";
        else if (!(e.image->width > 0.0f) || !(e.image->height > 0.0f)) {
            e.image.reset();
            e.error = "the SVG '" + path + "' has no size (width, height or viewBox)";
        }
        it = g_cache.emplace(path, std::move(e)).first;
    }
    if (!it->second.image) error = it->second.error;
    return it->second.image.get();
}

// nanosvg's colours are 0xAABBGGRR.
Color colour(unsigned int c, double opacity) {
    return Color{(c & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, ((c >> 16) & 0xFF) / 255.0,
                 ((c >> 24) & 0xFF) / 255.0 * opacity};
}

// A paint as one colour: a gradient becomes the mean of its stops (§75: a
// symbol's gradients are rare and subtle; the canvas has none yet).
std::optional<Color> paintColour(const NSVGpaint& p, double opacity) {
    if (p.type == NSVG_PAINT_COLOR) return colour(p.color, opacity);
    if ((p.type == NSVG_PAINT_LINEAR_GRADIENT || p.type == NSVG_PAINT_RADIAL_GRADIENT) && p.gradient &&
        p.gradient->nstops > 0) {
        double r = 0, g = 0, b = 0, a = 0;
        for (int k = 0; k < p.gradient->nstops; ++k) {
            const Color s = colour(p.gradient->stops[k].color, opacity);
            r += s.r; g += s.g; b += s.b; a += s.a;
        }
        const double n = p.gradient->nstops;
        return Color{r / n, g / n, b / n, a / n};
    }
    return std::nullopt;
}

void addPaths(Canvas& cv, const NSVGshape& shape, const Affine& m) {
    for (const NSVGpath* path = shape.paths; path; path = path->next) {
        if (path->npts < 1) continue;
        const float* p = path->pts;
        cv.moveTo(m.apply({p[0], p[1]}));
        for (int i = 1; i + 2 < path->npts; i += 3) {
            const float* q = p + i * 2;
            cv.curveTo(m.apply({q[0], q[1]}), m.apply({q[2], q[3]}), m.apply({q[4], q[5]}));
        }
        if (path->closed) cv.closePath();
    }
}

} // namespace

bool isSvgFile(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".svg";
}

std::optional<SvgSize> svgSize(const std::string& path, std::string& error) {
    const NSVGimage* image = load(path, error);
    if (!image) return std::nullopt;
    return SvgSize{image->width, image->height};
}

bool drawSvg(Canvas& cv, const std::string& path, const Affine& m, double scale, std::string& error) {
    const NSVGimage* image = load(path, error);
    if (!image) return false;
    for (const NSVGshape* shape = image->shapes; shape; shape = shape->next) {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;
        const std::optional<Color> fill = paintColour(shape->fill, shape->opacity);
        const std::optional<Color> stroke = shape->strokeWidth > 0.0f ? paintColour(shape->stroke, shape->opacity)
                                                                       : std::nullopt;
        if (!fill && !stroke) continue;
        StrokeStyle line;
        if (stroke) {
            line.color = *stroke;
            line.width = shape->strokeWidth * scale;
            line.cap   = shape->strokeLineCap == NSVG_CAP_ROUND    ? LineCap::Round
                       : shape->strokeLineCap == NSVG_CAP_SQUARE   ? LineCap::Square : LineCap::Butt;
            line.join  = shape->strokeLineJoin == NSVG_JOIN_ROUND  ? LineJoin::Round
                       : shape->strokeLineJoin == NSVG_JOIN_BEVEL  ? LineJoin::Bevel : LineJoin::Miter;
            for (int k = 0; k < shape->strokeDashCount; ++k) line.dashes.push_back(shape->strokeDashArray[k] * scale);
            line.dashOffset = shape->strokeDashOffset * scale;
        }
        // SVG's paint-order: the fill first, unless the stroke is said first.
        const bool strokeFirst = (shape->paintOrder & 0x03) == NSVG_PAINT_STROKE;
        FillStyle area;
        if (fill) {
            area.color = *fill;
            area.rule  = shape->fillRule == NSVG_FILLRULE_EVENODD ? FillRule::EvenOdd : FillRule::NonZero;
        }
        addPaths(cv, *shape, m);
        if (strokeFirst && stroke) cv.stroke(line, fill ? PathMode::Keep : PathMode::Consume);
        if (fill) cv.fill(area, (stroke && !strokeFirst) ? PathMode::Keep : PathMode::Consume);
        if (!strokeFirst && stroke) cv.stroke(line, PathMode::Consume);
    }
    return true;
}

} // namespace r2d
