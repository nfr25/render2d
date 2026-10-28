// SPDX-License-Identifier: AGPL-3.0-or-later
#include "render/query_layer.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

#include "render/hit_test.hpp"
#include "render/labels.hpp"
#include "render/rule_matcher.hpp"
#include "render/symbolize.hpp"

namespace r2d {

namespace {

// std::visit with one lambda per alternative (as in symbolize.cpp).
template <class... F> struct Overloaded : F... { using F::operator()...; };
template <class... F> Overloaded(F...) -> Overloaded<F...>;

double number(const Value& v, double fallback) {
    if (const auto* d = std::get_if<double>(&v))    return *d;
    if (const auto* i = std::get_if<long long>(&v)) return static_cast<double>(*i);
    return fallback;
}

// Does one symbolizer, drawing this geometry, reach the point? All in metres:
// `tol` is the tolerance, `res` the metres per image pixel. §52.
bool touches(const Symbolizer& sym, const BoundPoint* bp, const BoundText* bt, const Geometry& g,
             const Attributes& attrs, EvalStats& ev, Point p, double tol, double res) {
    return std::visit(Overloaded{
        // A filled polygon: inside (even-odd), or near its outline.
        [&](const PolygonSymbolizer& s) {
            if (g.type != GeomType::Polygon) return false;
            const bool filled = s.fill.has_value() || s.graphicFill.has_value();
            if (filled && insideEvenOdd(g, p)) return true;
            const double half = s.stroke ? s.stroke->width / 2.0 * res : 0.0;
            return distanceToParts(g, p, true) <= tol + half;
        },
        // A line, or the outline of a polygon: the distance to its segments.
        [&](const LineSymbolizer& s) {
            if (g.type == GeomType::Point) return false;
            return distanceToParts(g, p, g.type == GeomType::Polygon) <= tol + s.stroke.width / 2.0 * res;
        },
        // A point: its mark is a disc of half its size (in pixels, or metres).
        [&](const PointSymbolizer& s) {
            if (g.type != GeomType::Point) return false;
            double size = s.size > 0.0 ? s.size : 16.0;              // 0 = an image's own size: guess
            if (bp && bp->size) size = number(bp->size->value(attrs, ev), size);
            const double radius = s.metres ? size / 2.0 : size / 2.0 * res;
            const double half   = s.stroke ? s.stroke->width / 2.0 * res : 0.0;
            return distanceToPoints(g, p) <= tol + radius + half;
        },
        // A text: around its anchor, half a character high. §52.
        [&](const TextSymbolizer& s) {
            if (bt && bt->label && labelText(bt->label->value(attrs, ev)).empty()) return false;
            const double radius = s.font.size / 2.0 * res;
            for (const Point& a : labelPoints(g))
                if (std::hypot(p.x - a.x, p.y - a.y) <= tol + radius) return true;
            return false;
        },
        [](const RasterSymbolizer&) { return false; },   // a raster has no feature to find
    }, sym);
}

} // namespace

std::vector<LayerHit> queryLayer(FeatureSource& source, const View& view, Point world, double tolerancePx,
                                 const StyleByType& styles) {
    const double res   = view.resolution;
    const double tol   = tolerancePx * res;
    const double scale = view.scaleDenominator();

    // A small square around the point: the tolerance plus the widest symbol (§18).
    double reachPx = 0.0;
    std::size_t passes = 0;
    for (const Style* s : styles) {
        if (!s) continue;
        reachPx = std::max(reachPx, styleReachPx(*s));
        passes  = std::max(passes, s->featureStyles.size());
    }
    const double r = tol + reachPx * res;
    source.setSpatialFilter({world.x - r, world.y - r, world.x + r, world.y + r});

    // Every field: the hits are shown with all their attributes (§53).
    std::vector<int> all(source.fields().size());
    std::iota(all.begin(), all.end(), 0);

    std::vector<LayerHit> hits;     // in reading order
    std::set<FeatureId>   found;    // a feature drawn by two passes counts once
    std::vector<std::string> missing;
    EvalStats ev;
    for (std::size_t k = 0; k < passes; ++k) {
        RuleMatcher matcher(source.fields(), styles, k, scale, missing);   // the same as the drawing (§51)
        if (!matcher.any()) continue;
        source.setNeededFields(all);
        source.forEach([&](const Geometry& g, const Attributes& attrs, FeatureId fid) {
            if (found.count(fid) || !matcher.match(g.type, attrs, ev)) return;
            const ActiveRules& a = matcher.active(g.type);
            for (std::size_t i = 0; i < a.rules.size(); ++i) {
                if (!a.draws[i]) continue;
                const auto& syms = a.rules[i]->symbolizers;
                for (std::size_t j = 0; j < syms.size(); ++j) {
                    if (touches(syms[j], a.points[i][j], a.texts[i][j], g, attrs, ev, world, tol, res)) {
                        hits.push_back({fid, g, attrs});
                        found.insert(fid);
                        return;
                    }
                }
            }
        });
    }
    source.setNeededFields({});
    std::reverse(hits.begin(), hits.end());   // the last drawn is on top: first (§52)
    return hits;
}

std::vector<LayerHit> queryLayerArea(FeatureSource& source, const View& view, const Geometry& zone,
                                     const StyleByType& styles, std::size_t maxHits, bool& truncated) {
    std::vector<LayerHit> hits;
    if (zone.empty()) return hits;
    const double scale = view.scaleDenominator();

    std::size_t passes = 0;
    for (const Style* s : styles)
        if (s) passes = std::max(passes, s->featureStyles.size());

    // GDAL only knows rectangles: the zone's envelope (§18), then the exact test.
    source.setSpatialFilter(zone.envelope());

    std::vector<int> all(source.fields().size());
    std::iota(all.begin(), all.end(), 0);

    std::set<FeatureId>      found;
    std::vector<std::string> missing;
    EvalStats ev;
    for (std::size_t k = 0; k < passes && !truncated; ++k) {
        RuleMatcher matcher(source.fields(), styles, k, scale, missing);   // what is seen (§51)
        if (!matcher.any()) continue;
        source.setNeededFields(all);
        source.forEach([&](const Geometry& g, const Attributes& attrs, FeatureId fid) {
            if (truncated || found.count(fid)) return;
            if (!matcher.match(g.type, attrs, ev) || !touchesArea(g, zone)) return;
            if (hits.size() >= maxHits) {   // §54: one too many: copy no more
                truncated = true;
                return;
            }
            hits.push_back({fid, g, attrs});
            found.insert(fid);
        });
    }
    source.setNeededFields({});
    std::reverse(hits.begin(), hits.end());
    return hits;
}

} // namespace r2d
