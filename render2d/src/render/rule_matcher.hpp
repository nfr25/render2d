// SPDX-License-Identifier: AGPL-3.0-or-later
// Which rules draw a feature: the active rules of one pass at this scale, their
// filters bound to the layer's columns, the else rules. Taken out of
// renderLayer so that drawing and querying use the SAME code: "one selects
// only what one sees" (notions §51). See also §24, §28 and §29.
#pragma once

#include <array>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/attributes.hpp"
#include "core/geometry.hpp"
#include "render/render_layer.hpp"
#include "style/style.hpp"

namespace r2d {

// A symbolizer with expressions (a text, or a point with size or rotation),
// its expressions bound to this layer's columns.
struct BoundText {
    const TextSymbolizer*     sym = nullptr;
    std::optional<Expression> label, rotation;
};
struct BoundPoint {
    std::optional<Expression> size, rotation;
};

// The active rules of one pass for one geometry type, with what each needs
// per feature: its bound filter (or none), its texts and points.
struct ActiveRules {
    std::vector<const Rule*>                    rules;
    std::vector<const Expression*>              filters;   // parallel to rules; nullptr = no filter
    std::vector<std::vector<const BoundText*>>  texts;     // per rule, per symbolizer; nullptr = not a text
    std::vector<std::vector<const BoundPoint*>> points;    // per rule, per symbolizer; nullptr = no expression
    std::vector<char>                           draws;     // after match(): 1 = this rule draws the feature
};

class RuleMatcher {
public:
    // Pass `pass` of the styles (one per geometry type), at this scale. Field
    // names are bound to `fields` once; unknown names go to `missingFields`.
    RuleMatcher(const std::vector<FieldDef>& fields, const StyleByType& styles, std::size_t pass,
                double scaleDenominator, std::vector<std::string>& missingFields);

    RuleMatcher(const RuleMatcher&) = delete;
    RuleMatcher& operator=(const RuleMatcher&) = delete;

    // At least one rule active for one type: else the pass reads nothing.
    bool any() const { return any_; }

    // The columns the expressions read (§30). May contain duplicates.
    const std::vector<int>& columns() const { return columns_; }

    ActiveRules& active(GeomType t) { return active_[static_cast<int>(t)]; }

    // §28: every rule whose filter is true draws; the else rules only when no
    // other rule did. Fills active(t).draws; true when at least one rule draws.
    bool match(GeomType t, const Attributes& attrs, EvalStats& evalStats);

private:
    const Expression* bind(const Expression& e);
    const Expression* filterOf(const Rule* r);
    const BoundText*  textOf(const TextSymbolizer* t);
    const BoundPoint* pointOf(const PointSymbolizer* p);

    const std::vector<FieldDef>&  fields_;
    std::vector<std::string>&     missing_;
    std::array<ActiveRules, 3>    active_;
    std::vector<int>              columns_;
    bool                          any_ = false;

    std::deque<Expression>                              exprs_;   // deque: addresses stay valid
    std::deque<BoundText>                               textsStore_;
    std::deque<BoundPoint>                              pointsStore_;
    std::map<const Rule*, const Expression*>            filters_; // a style shared by 3 types: bind once
    std::map<const TextSymbolizer*, const BoundText*>   texts_;
    std::map<const PointSymbolizer*, const BoundPoint*> points_;
};

} // namespace r2d
