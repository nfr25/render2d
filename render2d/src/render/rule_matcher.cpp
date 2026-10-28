// SPDX-License-Identifier: AGPL-3.0-or-later
#include "render/rule_matcher.hpp"

#include <algorithm>

namespace r2d {

namespace {

void addUnique(std::vector<std::string>& v, const std::string& s) {
    if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

} // namespace

RuleMatcher::RuleMatcher(const std::vector<FieldDef>& fields, const StyleByType& styles, std::size_t pass,
                         double scaleDenominator, std::vector<std::string>& missingFields)
    : fields_(fields), missing_(missingFields) {
    // Rules active at this scale, per geometry type: decided once per pass. §24.
    // Expressions are bound once per pass too (names -> columns). §29.
    for (int t = 0; t < 3; ++t) {
        const Style* s = styles[t];
        if (!s || pass >= s->featureStyles.size()) continue;
        ActiveRules& a = active_[t];
        for (const Rule* r : activeRules(s->featureStyles[pass], scaleDenominator)) {
            std::vector<const BoundText*>  texts;
            std::vector<const BoundPoint*> points;
            for (const Symbolizer& sym : r->symbolizers) {
                const auto* ts = std::get_if<TextSymbolizer>(&sym);
                const auto* ps = std::get_if<PointSymbolizer>(&sym);
                texts.push_back(ts ? textOf(ts) : nullptr);
                points.push_back(ps ? pointOf(ps) : nullptr);
            }
            a.rules.push_back(r);
            a.filters.push_back(r->filter ? filterOf(r) : nullptr);
            a.texts.push_back(std::move(texts));
            a.points.push_back(std::move(points));
        }
        a.draws.resize(a.rules.size());
        any_ = any_ || !a.rules.empty();
    }
}

bool RuleMatcher::match(GeomType t, const Attributes& attrs, EvalStats& evalStats) {
    ActiveRules& a = active(t);
    const std::size_t n = a.rules.size();
    bool anyMatched = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (a.rules[i]->isElse) { a.draws[i] = 0; continue; }
        const bool ok = !a.filters[i] || a.filters[i]->test(attrs, evalStats) == Tri::True;
        a.draws[i] = ok;
        anyMatched = anyMatched || ok;
    }
    bool anyDraws = anyMatched;
    if (!anyMatched) {
        for (std::size_t i = 0; i < n; ++i) {
            if (a.rules[i]->isElse) { a.draws[i] = 1; anyDraws = true; }
        }
    }
    return anyDraws;
}

const Expression* RuleMatcher::bind(const Expression& e) {
    exprs_.push_back(e);
    Expression& b = exprs_.back();
    for (const auto& m : b.bind(fields_)) addUnique(missing_, m);
    for (int c : b.columns()) columns_.push_back(c);
    return &b;
}

const Expression* RuleMatcher::filterOf(const Rule* r) {
    auto it = filters_.find(r);
    if (it == filters_.end()) it = filters_.emplace(r, bind(*r->filter)).first;
    return it->second;
}

const BoundText* RuleMatcher::textOf(const TextSymbolizer* t) {
    auto it = texts_.find(t);
    if (it != texts_.end()) return it->second;
    textsStore_.push_back(BoundText{t, std::nullopt, std::nullopt});
    BoundText& b = textsStore_.back();
    if (t->label)    b.label    = *bind(*t->label);
    if (t->rotation) b.rotation = *bind(*t->rotation);
    return texts_.emplace(t, &b).first->second;
}

const BoundPoint* RuleMatcher::pointOf(const PointSymbolizer* p) {
    if (!p->sizeExpr && !p->rotation) return nullptr;
    auto it = points_.find(p);
    if (it != points_.end()) return it->second;
    pointsStore_.push_back(BoundPoint{});
    BoundPoint& b = pointsStore_.back();
    if (p->sizeExpr) b.size     = *bind(*p->sizeExpr);
    if (p->rotation) b.rotation = *bind(*p->rotation);
    return points_.emplace(p, &b).first->second;
}

} // namespace r2d
