// SPDX-License-Identifier: AGPL-3.0-or-later
#include "render/render_layer.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <vector>

#include "render/rule_matcher.hpp"
#include "render/symbolize.hpp"

namespace r2d {

namespace {

void addUnique(std::vector<std::string>& v, const std::string& s) {
    if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

// A value as a number; NULL or a text gives `fallback`.
double number(const Value& v, double fallback) {
    if (const auto* d = std::get_if<double>(&v))     return *d;
    if (const auto* i = std::get_if<long long>(&v))  return static_cast<double>(*i);
    return fallback;
}

} // namespace

LayerRenderStats renderLayer(FeatureSource& source, Canvas& cv, const View& view,
                             const StyleByType& styles, std::vector<Label>* labels) {
    using Clock = std::chrono::steady_clock;
    LayerRenderStats stats;
    const double scale = view.scaleDenominator();
    std::vector<Label> own;                 // no collector given: draw this layer's labels at the end
    std::vector<Label>& out = labels ? *labels : own;

    // 1. Spatial filter: shown area + the reach of the widest symbol. §18, §37.
    double reachPx = 0.0;
    std::size_t passes = 0;
    for (const Style* s : styles) {
        if (!s) continue;
        reachPx = std::max(reachPx, styleReachPx(*s));
        passes  = std::max(passes, s->featureStyles.size());
        for (const auto& fts : s->featureStyles)
            for (const Rule& r : fts.rules) {
                stats.filtered = stats.filtered || r.filter || r.isElse;
                for (const auto& sym : r.symbolizers) {
                    stats.filtered = stats.filtered || std::holds_alternative<TextSymbolizer>(sym);
                    if (const auto* p = std::get_if<PointSymbolizer>(&sym))
                        stats.filtered = stats.filtered || p->sizeExpr || p->rotation;
                }
            }
    }
    source.setSpatialFilter(view.shown.expandedBy(reachPx * view.resolution));

    std::map<const Rule*, std::size_t> drawnBy;   // stable addresses for the counters
    EvalStats evalStats;
    std::map<const void*, int> patterns;          // §43: hatching tiles, made once per layer
    SymbolContext shared;
    shared.patterns = &patterns;
    shared.errors   = &stats.imageErrors;

    // 2. One pass per feature-style, in order. §25.
    for (std::size_t k = 0; k < passes; ++k) {
        // The rules active at this scale and their bound expressions: the same
        // RuleMatcher as the queries (§51), so that what is drawn is what is selectable.
        RuleMatcher matcher(source.fields(), styles, k, scale, stats.missingFields);
        if (!matcher.any()) continue;   // nothing to draw at this scale: do not even read

        // A counter per active rule, for the report.
        std::array<std::vector<std::size_t*>, 3> counts;
        for (int t = 0; t < 3; ++t)
            for (const Rule* r : matcher.active(static_cast<GeomType>(t)).rules) counts[t].push_back(&drawnBy[r]);

        // §30: only the columns the expressions use; none at all without them.
        source.setNeededFields(matcher.columns());
        for (int c : matcher.columns()) addUnique(stats.fieldsRead, source.fields()[c].name);

        Clock::duration drawTime{}, filterTime{};
        const auto t0 = Clock::now();
        stats.read = source.forEach([&](const Geometry& g, const Attributes& attrs, FeatureId) {
            ActiveRules& a = matcher.active(g.type);
            const std::size_t n = a.rules.size();
            if (n == 0) return;

            // Which rules apply: every rule whose filter is true; the else
            // rules only when no other rule applied. §28.
            const auto f0 = Clock::now();
            matcher.match(g.type, attrs, evalStats);
            auto d0 = f0;                        // without filters: no extra clock reading
            if (stats.filtered) { d0 = Clock::now(); filterTime += d0 - f0; }

            for (std::size_t i = 0; i < n; ++i) {                       // file order
                if (!a.draws[i]) continue;
                ++*counts[static_cast<int>(g.type)][i];
                const auto& syms = a.rules[i]->symbolizers;
                for (std::size_t j = 0; j < syms.size(); ++j) {
                    const BoundText* bt = a.texts[i][j];
                    if (!bt) {
                        SymbolContext ctx = shared;
                        if (const BoundPoint* bp = a.points[i][j]) {   // §41: size and rotation per feature
                            if (bp->size)     ctx.size     = number(bp->size->value(attrs, evalStats), -1.0);
                            if (bp->rotation) ctx.rotation = number(bp->rotation->value(attrs, evalStats), 0.0);
                        }
                        drawSymbolizer(cv, view.worldToImage, g, syms[j], ctx);
                        continue;
                    }

                    // §35: a label is computed now, drawn after all the layers.
                    const std::string text = bt->label ? labelText(bt->label->value(attrs, evalStats)) : "";
                    if (text.empty()) { ++stats.labelsEmpty; continue; }
                    const double rotation = bt->rotation ? number(bt->rotation->value(attrs, evalStats), 0.0) : 0.0;
                    const TextSymbolizer& ts = *bt->sym;
                    for (const Point& p : labelPoints(g)) {
                        out.push_back(Label{text, ts.font, ts.fill, ts.halo, view.worldToImage.apply(p),
                                            ts.anchorX, ts.anchorY, ts.displacementX, ts.displacementY,
                                            readableAngle(rotation)});
                        ++stats.labels;
                    }
                }
            }
            drawTime += Clock::now() - d0;
        });
        const auto total = Clock::now() - t0;

        ++stats.passes;
        stats.drawMs   += std::chrono::duration<double, std::milli>(drawTime).count();
        stats.filterMs += std::chrono::duration<double, std::milli>(filterTime).count();
        stats.readMs   += std::chrono::duration<double, std::milli>(total - drawTime - filterTime).count();
    }
    source.setNeededFields({});
    stats.notNumber = evalStats.notNumber;
    if (!labels) drawLabels(cv, own);

    // 3. Counters per rule, in file order, for the report.
    if (stats.filtered) {
        const Style* seen[3] = {nullptr, nullptr, nullptr};
        for (int t = 0; t < 3; ++t) {
            const Style* s = styles[t];
            if (!s || std::find(seen, seen + 3, s) != seen + 3) continue;
            seen[t] = s;
            for (std::size_t p = 0; p < s->featureStyles.size(); ++p) {
                const auto& rules = s->featureStyles[p].rules;
                for (std::size_t i = 0; i < rules.size(); ++i) {
                    const auto it = drawnBy.find(&rules[i]);
                    if (it == drawnBy.end()) continue;
                    std::string name = rules[i].name;
                    if (name.empty()) name = "pass " + std::to_string(p + 1) + " rule " + std::to_string(i + 1);
                    stats.rules.push_back({name, it->second});
                }
            }
        }
    }
    return stats;
}

} // namespace r2d
