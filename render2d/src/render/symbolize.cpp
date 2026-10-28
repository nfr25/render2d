// SPDX-License-Identifier: AGPL-3.0-or-later
#include "render/symbolize.hpp"

#include "style/catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "canvas/shapes.hpp"

namespace r2d {

namespace {

bool samePoint(Point a, Point b) { return a.x == b.x && a.y == b.y; }

// One closed ring. The last vertex usually repeats the first (Simple Features):
// skip it and let closePath close, for a clean join at the start. notions §19.
void addRing(Canvas& cv, const Affine& m, const Geometry& g, const Part& part) {
    if (part.count == 0) return;
    const Point* p = &g.points[part.start];
    std::size_t n = part.count;
    if (n > 1 && samePoint(p[0], p[n - 1])) --n;

    cv.moveTo(m.apply(p[0]));
    for (std::size_t i = 1; i < n; ++i) cv.lineTo(m.apply(p[i]));
    cv.closePath();
}

void addLine(Canvas& cv, const Affine& m, const Geometry& g, const Part& part) {
    if (part.count < 2) return;
    const Point* p = &g.points[part.start];
    cv.moveTo(m.apply(p[0]));
    for (std::size_t i = 1; i < part.count; ++i) cv.lineTo(m.apply(p[i]));
}

// Fill and/or stroke the current path, whichever is present.
void paint(Canvas& cv, const std::optional<FillStyle>& fill, const std::optional<StrokeStyle>& stroke) {
    if (fill && stroke)  cv.fillAndStroke(*fill, *stroke);
    else if (fill)       cv.fill(*fill, PathMode::Consume);
    else if (stroke)     cv.stroke(*stroke, PathMode::Consume);
}

// std::visit helper: one lambda per symbolizer type.
template <class... F> struct Overloaded : F... { using F::operator()...; };
template <class... F> Overloaded(F...) -> Overloaded<F...>;

} // namespace

namespace {

constexpr double kPi = 3.14159265358979323846;

bool isLineShape(MarkShape s) {
    return s == MarkShape::Slash || s == MarkShape::Backslash || s == MarkShape::HorLine ||
           s == MarkShape::VertLine || s == MarkShape::Times;
}

// A well-known mark, from its -1..+1 frame through `m`. §40.
void addMark(Canvas& cv, MarkShape shape, const Affine& m) {
    switch (shape) {
        case MarkShape::Square:   marks::square(cv, m);   break;
        case MarkShape::Triangle: marks::triangle(cv, m); break;
        case MarkShape::Star:     marks::star(cv, m);     break;
        case MarkShape::Cross:    marks::cross(cv, m);    break;
        case MarkShape::X: {
            const double c = std::sqrt(0.5);
            marks::cross(cv, m * Affine{c, c, -c, c, 0.0, 0.0});   // the cross turned by 45 degrees
            break;
        }
        case MarkShape::Slash:    cv.moveTo(m.apply({-1, -1})); cv.lineTo(m.apply({1, 1}));  break;
        case MarkShape::Backslash: cv.moveTo(m.apply({-1, 1})); cv.lineTo(m.apply({1, -1})); break;
        case MarkShape::HorLine:  cv.moveTo(m.apply({-1, 0}));  cv.lineTo(m.apply({1, 0}));  break;
        case MarkShape::VertLine: cv.moveTo(m.apply({0, -1}));  cv.lineTo(m.apply({0, 1}));  break;
        case MarkShape::Times:
            cv.moveTo(m.apply({-1, -1})); cv.lineTo(m.apply({1, 1}));
            cv.moveTo(m.apply({-1, 1}));  cv.lineTo(m.apply({1, -1}));
            break;
        case MarkShape::Dot: {
            marks::circle(cv, m * Affine::scaling(0.25, 0.25));
            break;
        }
        case MarkShape::Circle:   marks::circle(cv, m);   break;
    }
}

// Image pixels per world metre, from the world -> image matrix (no shear).
double pixelsPerMetre(const Affine& m) { return std::sqrt(std::abs(m.determinant())); }

// Position, rotation (clockwise on the image: y goes down) and scale. §40.
Affine place(Point c, double rotationDeg, double sx, double sy) {
    const double t = rotationDeg * kPi / 180.0;
    const Affine rotate{std::cos(t), std::sin(t), -std::sin(t), std::cos(t), 0.0, 0.0};
    return Affine::translation(c.x, c.y) * rotate * Affine::scaling(sx, sy);
}

// One tile of a hatching. The diagonals are drawn three times, shifted by
// one tile left and right, so that their ends meet across tiles. §43.
void drawTile(Canvas& t, const GraphicFill& gf, int n) {
    const double h = n / 2.0;
    const Affine center = Affine::translation(h, h) * Affine::scaling(h, -h);   // -1..+1, y up
    const bool diagonal = gf.shape == MarkShape::Slash || gf.shape == MarkShape::Backslash ||
                          gf.shape == MarkShape::Times;
    for (int d = diagonal ? -1 : 0; d <= (diagonal ? 1 : 0); ++d)
        addMark(t, gf.shape, Affine::translation(d * n, 0.0) * center);
    if (isLineShape(gf.shape)) {
        if (gf.stroke) t.stroke(*gf.stroke, PathMode::Consume);
        return;
    }
    if (gf.fill && gf.stroke) t.fillAndStroke(*gf.fill, *gf.stroke);
    else if (gf.fill)         t.fill(*gf.fill, PathMode::Consume);
    else if (gf.stroke)       t.stroke(*gf.stroke, PathMode::Consume);
    else                      t.fill(FillStyle{.color = Color::fromHex(0x808080)}, PathMode::Consume);
}

} // namespace

void drawSymbolizer(Canvas& cv, const Affine& m, const Geometry& g, const Symbolizer& sym,
                    const SymbolContext& ctx) {
    std::visit(Overloaded{
        [&](const PolygonSymbolizer& s) {
            if (g.type != GeomType::Polygon || (!s.fill && !s.stroke && !s.graphicFill)) return;
            for (const Part& part : g.parts) addRing(cv, m, g, part);
            if (!s.graphicFill) { paint(cv, s.fill, s.stroke); return; }
            if (s.fill) cv.fill(*s.fill, PathMode::Keep);
            {
                // §43: one tile per symbolizer and render, anchored to the world origin.
                const int n = std::max(1, static_cast<int>(std::lround(s.graphicFill->size)));
                int handle = -1;
                if (ctx.patterns) {
                    auto it = ctx.patterns->find(&s);
                    if (it == ctx.patterns->end())
                        it = ctx.patterns->emplace(&s, cv.createPattern(n, [&](Canvas& t) { drawTile(t, *s.graphicFill, n); })).first;
                    handle = it->second;
                } else {
                    handle = cv.createPattern(n, [&](Canvas& t) { drawTile(t, *s.graphicFill, n); });
                }
                // The world origin in pixels is far away (Lambert 72: about -450 000 px
                // at 0.33 m/px), beyond what Cairo's pixman can hold (16.16 fixed point,
                // +-32 768). The tiling repeats every n pixels: keep it modulo n. §43.
                const Point o = m.apply({0.0, 0.0});
                cv.fillPattern(handle, {std::fmod(o.x, n), std::fmod(o.y, n)},
                               s.stroke ? PathMode::Keep : PathMode::Consume);
            }
            if (s.stroke) cv.stroke(*s.stroke, PathMode::Consume);
        },
        [&](const LineSymbolizer& s) {
            if (g.type == GeomType::Line) {
                for (const Part& part : g.parts) addLine(cv, m, g, part);
            } else if (g.type == GeomType::Polygon) {
                for (const Part& part : g.parts) addRing(cv, m, g, part);   // outline
            } else {
                return;
            }
            cv.stroke(s.stroke, PathMode::Consume);
        },
        [&](const PointSymbolizer& s) {
            if (g.type != GeomType::Point) return;
            double size = ctx.size >= 0.0 ? ctx.size : s.size;
            if (s.metres) size *= pixelsPerMetre(m);                       // §41: metres -> pixels
            if (!(size >= 0.0)) return;

            // §42: the image first; when it cannot be read, the mark.
            if (!s.image.empty()) {
                std::string error;
                const int handle = cv.loadImage(s.image, error);
                if (handle >= 0) {
                    const ImageInfo info = cv.imageInfo(handle);
                    const double k = size > 0.0 && info.height > 0 ? size / info.height : 1.0;
                    for (const Point& p : g.points)
                        cv.drawImage(handle, place(m.apply(p), ctx.rotation, k, k) *
                                             Affine::translation(-info.width / 2.0, -info.height / 2.0));
                    return;
                }
                if (ctx.errors && std::find(ctx.errors->begin(), ctx.errors->end(), error) == ctx.errors->end())
                    ctx.errors->push_back(error);
            }
            if (!s.fill && !s.stroke) return;
            const double r = size / 2.0;
            for (const Point& p : g.points) addMark(cv, s.shape, place(m.apply(p), ctx.rotation, r, -r));
            if (isLineShape(s.shape)) { if (s.stroke) cv.stroke(*s.stroke, PathMode::Consume); return; }
            paint(cv, s.fill, s.stroke);
        },
        [](const TextSymbolizer&) {},   // labels are collected by renderLayer, drawn at the end
        [](const RasterSymbolizer&) {}, // a raster layer is drawn by Map (§66), not feature by feature
    }, sym);
}

double symbolizerReachPx(const Symbolizer& sym) {
    return std::visit(Overloaded{
        [](const PolygonSymbolizer& s) { return s.stroke ? s.stroke->width / 2.0 : 0.0; },
        [](const LineSymbolizer& s)    { return s.stroke.width / 2.0; },
        [](const PointSymbolizer& s) {
            // A size from an expression or in metres is not known here: count 64 pixels.
            const double size = (s.sizeExpr || s.metres) ? std::max(s.size, 64.0) : (s.size > 0 ? s.size : 64.0);
            return size / 2.0 * (s.rotation || s.shape != MarkShape::Circle ? std::sqrt(2.0) : 1.0) +
                   (s.stroke ? s.stroke->width / 2.0 : 0.0);
        },
        // §37: the text is not known before reading. Estimate: 12 characters
        // of 0.6 em, plus the halo and the displacement, in any direction.
        [](const TextSymbolizer& s) {
            return 12.0 * 0.6 * s.font.size + (s.halo ? s.halo->radius : 0.0) +
                   std::hypot(s.displacementX, s.displacementY);
        },
        [](const RasterSymbolizer&) { return 0.0; },
    }, sym);
}

double styleReachPx(const Style& style) {
    double reach = 0.0;
    for (const auto& fts : style.featureStyles)
        for (const auto& rule : fts.rules)
            for (const auto& sym : rule.symbolizers) reach = std::max(reach, symbolizerReachPx(sym));
    return reach;
}

Style scaledStyle(const Style& style, double k) {
    Style out = style;
    auto stroke = [k](StrokeStyle& s) {
        s.width *= k;
        for (double& d : s.dashes) d *= k;
        s.dashOffset *= k;
    };
    for (auto& fts : out.featureStyles)
        for (auto& rule : fts.rules)
            for (auto& sym : rule.symbolizers)
                std::visit(Overloaded{
                    [&](PolygonSymbolizer& p) {
                        if (p.stroke) stroke(*p.stroke);
                        if (p.graphicFill) {
                            p.graphicFill->size *= k;
                            if (p.graphicFill->stroke) stroke(*p.graphicFill->stroke);
                        }
                    },
                    [&](LineSymbolizer& l)    { stroke(l.stroke); },
                    [&](PointSymbolizer& p)   {
                        if (!p.metres) p.size *= k;      // metres are already a real size (§41)
                        if (p.stroke) stroke(*p.stroke);
                    },
                    [&](TextSymbolizer& t) {
                        t.font.size *= k;
                        if (t.halo) t.halo->radius *= k;
                        t.displacementX *= k;
                        t.displacementY *= k;
                    },
                    [](RasterSymbolizer&) {},   // pixels stay pixels
                }, sym);
    return out;
}

Style defaultStyle(GeomType type, int layerIndex) {
    // The look lives in the catalogue (§60), shared with editor2d's "+".
    const SymbolizerKind kind = type == GeomType::Polygon ? SymbolizerKind::Polygon
                              : type == GeomType::Line    ? SymbolizerKind::Line
                                                          : SymbolizerKind::Point;
    const Symbolizer sym = newSymbolizer(kind, layerIndex);
    Rule rule;
    rule.name = "default";
    rule.symbolizers.push_back(sym);
    Style s;
    s.name = "default";
    s.featureStyles.push_back(FeatureTypeStyle{{rule}});
    return s;
}

} // namespace r2d
