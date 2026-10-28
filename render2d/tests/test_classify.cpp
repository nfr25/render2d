// SPDX-License-Identifier: AGPL-3.0-or-later
// Classifying a layer (notions §62; editor2d §48, §49): the numbers of the
// notions, then the rules read back by the parser, and every value in ONE class.

#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "style/classify.hpp"
#include "style/ysld_reader.hpp"
#include "style/ysld_writer.hpp"

using namespace r2d;

namespace {

// An in-memory layer: one column, no geometry needed.
class Column final : public FeatureSource {
public:
    Column(FieldDef def, std::vector<Value> values) : fields_{std::move(def)}, values_(std::move(values)) {}
    const std::vector<FieldDef>& fields() const override { return fields_; }
    void setNeededFields(const std::vector<int>&) override {}
    void setSpatialFilter(const Envelope&) override {}
    ReadStats forEach(const FeatureVisitor& visit) override {
        ReadStats s;
        Geometry g;
        for (std::size_t i = 0; i < values_.size(); ++i) {
            ++s.read;
            visit(g, Attributes{values_[i]}, static_cast<FeatureId>(i));
        }
        return s;
    }
private:
    std::vector<FieldDef> fields_;
    std::vector<Value>    values_;
};

// The ten surfaces of editor2d §49.
const std::vector<double> kAreas = {30, 45, 60, 80, 110, 150, 220, 300, 450, 900};

int hexOf(const Color& c) {
    auto b = [](double v) { return static_cast<int>(v * 255.0 + 0.5); };
    return (b(c.r) << 16) | (b(c.g) << 8) | b(c.b);
}

// How many rules of `rules` say true for this row (the else rule aside).
int matching(std::vector<Rule>& rules, const std::vector<FieldDef>& fields, const Attributes& row) {
    int n = 0;
    EvalStats st;
    for (Rule& r : rules) {
        if (r.isElse || !r.filter) continue;
        r.filter->bind(fields);
        if (r.filter->test(row, st) == Tri::True) ++n;
    }
    return n;
}

} // namespace

int main() {
    // §49: equal intervals, 320 and 610.
    std::vector<double> b = classBreaks(kAreas, 3, BreakMethod::EqualIntervals);
    CHECK(b.size() == 2);
    if (b.size() == 2) { CHECK_NEAR(b[0], 320, 1e-9); CHECK_NEAR(b[1], 610, 1e-9); }
    // §49: quantiles, ranks 3 and 6: 80 and 220.
    b = classBreaks(kAreas, 3, BreakMethod::Quantiles);
    CHECK(b.size() == 2);
    if (b.size() == 2) { CHECK(b[0] == 80); CHECK(b[1] == 220); }
    // Many equal values: fewer classes, none empty.
    // Ranks 2, 4, 6 of 8: 5 (the minimum: dropped), 9, 9 (twice: once).
    b = classBreaks({5, 5, 5, 5, 9, 9, 9, 9}, 4, BreakMethod::Quantiles);
    CHECK(b.size() == 1 && b[0] == 9);
    CHECK(classBreaks({7, 7, 7}, 3, BreakMethod::EqualIntervals).empty());   // one class
    // Bounds rounded to 6 significant digits: no 606.8399999999999 in a legend.
    b = classBreaks({70.4, 1412.5}, 5, BreakMethod::EqualIntervals);
    CHECK(b.size() == 4 && b[1] == 607.24);

    // §49: the colour in the middle, #C69178.
    const Color from = Color::fromHex(0xFFF5EB), to = Color::fromHex(0x8C2D04);
    CHECK(hexOf(rampColour(from, to, 0.5)) == 0xC69178);
    CHECK(hexOf(rampColour(from, to, 0.0)) == 0xFFF5EB);
    CHECK(hexOf(rampColour(from, to, 1.0)) == 0x8C2D04);
    CHECK(hexOf(categoryColour(0)) == 0x8DD3C7 && hexOf(categoryColour(12)) == 0x8DD3C7);

    // Graduate: three classes plus (other); every surface in exactly one class.
    const std::vector<FieldDef> fields = {{"AREA", FieldType::Real}};
    PolygonSymbolizer model;
    model.fill   = FillStyle{.color = Color::fromHex(0xC98B6B)};
    model.stroke = StrokeStyle{.color = Color::fromHex(0x7A4A35), .width = 0.8};
    std::vector<Rule> rules = graduate(model, "AREA", {80, 220}, from, to);
    CHECK(rules.size() == 4 && rules.back().isElse);
    for (double a : kAreas) CHECK(matching(rules, fields, {Value(a)}) == 1);
    CHECK(matching(rules, fields, {Value(5.0)}) == 1);       // below the minimum: class 1
    CHECK(matching(rules, fields, {Value(5000.0)}) == 1);    // above the maximum: the last
    CHECK(matching(rules, fields, {Value()}) == 0);          // no value: (other)
    CHECK(rules[0].name == "< 80" && rules[1].name == "80 - 220" && rules[2].name == ">= 220");
    // The model is kept, only its fill changes.
    const auto& p = std::get<PolygonSymbolizer>(rules[1].symbolizers[0]);
    CHECK(p.stroke && p.stroke->width == 0.8);
    CHECK(hexOf(p.fill->color) == 0xC69178);

    // Written and read again: the parser accepts every filter (§59's round trip).
    Style s;
    s.featureStyles.push_back({rules});
    const YsldResult back = parseYsld(writeYsld(s));
    CHECK(back.style && back.warnings.empty());
    if (back.style) CHECK(back.style->featureStyles[0].rules.size() == 4);

    // Summarize: a text field, distinct values sorted, the nulls counted.
    Column types({"TYPE", FieldType::Text},
                 {Value(std::string("maison")), Value(std::string("immeuble")), Value(std::string("maison")),
                  Value(), Value(std::string("l'annexe"))});
    FieldSummary sum = summarizeField(types, "TYPE");
    CHECK(sum.found && sum.total == 5 && sum.nulls == 1 && !sum.tooMany);
    CHECK(sum.distinct.size() == 3);
    if (sum.distinct.size() == 3) {
        CHECK(sum.distinct[0].first == "immeuble" && sum.distinct[2].first == "maison");
        CHECK(sum.distinct[2].second == 2);
    }
    CHECK(!summarizeField(types, "NOPE").found);

    // Categorize: a quote in a value, written as ''.
    std::vector<std::string> values;
    for (const auto& d : sum.distinct) values.push_back(d.first);
    std::vector<Rule> cats = categorize(model, "TYPE", FieldType::Text, values);
    CHECK(cats.size() == 4 && cats.back().isElse && cats.back().name == "(other)");
    const std::vector<FieldDef> tf = {{"TYPE", FieldType::Text}};
    CHECK(matching(cats, tf, {Value(std::string("l'annexe"))}) == 1);
    CHECK(matching(cats, tf, {Value(std::string("garage"))}) == 0);   // new value: (other)
    CHECK(filterLiteral("l'annexe", FieldType::Text) == "'l''annexe'");
    CHECK(filterLiteral("12", FieldType::Integer) == "12");

    // Numbers: sorted as numbers (9 before 10), the cap.
    Column nums({"N", FieldType::Integer}, {Value(10LL), Value(9LL), Value(10LL)});
    sum = summarizeField(nums, "N", 1);
    CHECK(sum.tooMany && sum.distinct.size() == 1 && sum.distinct[0].first == "9");
    CHECK(sum.numbers.size() == 3 && sum.numbers.front() == 9);

    // classify(): the whole assistant (§63). A base style: a hatched polygon
    // with a label, visible up to 1:5000.
    {
        Rule base;
        base.name        = "default";
        base.scale.max   = 5000;
        PolygonSymbolizer hatched = model;
        hatched.graphicFill = GraphicFill{};
        TextSymbolizer label;
        label.label = Expression::literalText("A");
        base.symbolizers = {hatched, label};
        Style st;
        st.featureStyles.push_back({{base}});

        std::vector<Value> vals;
        for (double a : kAreas) vals.push_back(Value(a));
        vals.push_back(Value());   // one without value
        Column col({"AREA", FieldType::Real}, vals);
        const FieldSummary fs = summarizeField(col, "AREA");

        ClassifySettings set;
        set.graduate = true;
        set.field    = "AREA";
        set.classes  = 3;
        set.method   = BreakMethod::Quantiles;
        const Classified c = classify(st, PolygonSymbolizer{}, fs, set);
        CHECK(c.ok);
        const auto& rs = c.style.featureStyles[0].rules;
        CHECK(rs.size() == 5);   // 3 classes, (other), labels
        if (rs.size() == 5) {
            CHECK(rs[4].name == "labels" && std::holds_alternative<TextSymbolizer>(rs[4].symbolizers[0]));
            CHECK(rs[1].scale.max == 5000 && rs[4].scale.max == 5000);   // the scales kept
            CHECK(std::get<PolygonSymbolizer>(rs[1].symbolizers[0]).graphicFill.has_value());   // the hatch kept
        }
        // The legend: 3, 3, 4 (§49), and the one without value in (other).
        CHECK(c.legend.size() == 4);
        if (c.legend.size() == 4) {
            CHECK(c.legend[0].count == 3 && c.legend[1].count == 3 && c.legend[2].count == 4);
            CHECK(c.legend[3].count == 1 && c.legend[3].name == "(other)");
            CHECK(hexOf(c.legend[1].colour) == 0xC69178);
        }
        // Two bands (editor2d §50): "proche" with an outline up to 1:15 000,
        // "loin" without beyond. The classes are repeated in each, with its symbol.
        Rule proche, loin;
        proche.name = "proche";
        proche.scale.max = 15000;
        proche.symbolizers = {model};          // fill + outline
        loin.name = "loin";
        loin.scale.min = 15000;
        PolygonSymbolizer fillOnly;
        fillOnly.fill = FillStyle{.color = Color::fromHex(0x8E7B6B)};
        loin.symbolizers = {fillOnly};
        Style two;
        two.featureStyles.push_back({{proche, loin}});
        set.graduate = true;
        set.maxDistinct = 50;
        const Classified c2 = classify(two, PolygonSymbolizer{}, fs, set);
        const auto& r2 = c2.style.featureStyles[0].rules;
        CHECK(c2.ok && r2.size() == 8);   // (3 classes + other) x 2 bands
        if (r2.size() == 8) {
            CHECK(r2[0].name == "< 80 (proche)" && r2[0].scale.max == 15000);
            CHECK(r2[4].name == "< 80 (loin)" && r2[4].scale.min == 15000);
            CHECK(std::get<PolygonSymbolizer>(r2[1].symbolizers[0]).stroke.has_value());    // proche: the outline
            CHECK(!std::get<PolygonSymbolizer>(r2[5].symbolizers[0]).stroke.has_value());   // loin: none
            CHECK(hexOf(std::get<PolygonSymbolizer>(r2[5].symbolizers[0]).fill->color) == 0xC69178);   // same colours
            CHECK(r2[3].isElse && r2[7].isElse);
        }
        CHECK(c2.legend.size() == 4 && c2.legend[0].name == "< 80");   // the legend: once, no suffix

        // Classified again (23 October, on UrbIS: "< 48 (< 29684 (proche))"):
        // the band names are found back, not nested.
        set.method = BreakMethod::EqualIntervals;
        const Classified c3 = classify(c2.style, PolygonSymbolizer{}, fs, set);
        const auto& r3 = c3.style.featureStyles[0].rules;
        CHECK(c3.ok && r3.size() == 8);
        if (r3.size() == 8) {
            CHECK(r3[0].name == "< 320 (proche)");
            CHECK(r3[4].name == "< 320 (loin)");
            CHECK(r3[3].name == "(other) (proche)");
        }
        set.method = BreakMethod::Quantiles;

        // Two rules of the SAME scale (a filter split): one band, the first is the model.
        Rule grands = proche, petits = proche;
        grands.name = "grands";
        petits.name = "petits";
        Style split;
        split.featureStyles.push_back({{grands, petits}});
        CHECK(classify(split, PolygonSymbolizer{}, fs, set).style.featureStyles[0].rules.size() == 4);

        // Refusals, said.
        set.graduate = false;
        set.maxDistinct = 5;
        const FieldSummary many = summarizeField(col, "AREA", 5);
        CHECK(!classify(st, PolygonSymbolizer{}, many, set).ok);   // 10 values > 5
        set.graduate = true;
        Column words({"T", FieldType::Text}, {Value(std::string("a"))});
        CHECK(!classify(st, PolygonSymbolizer{}, summarizeField(words, "T"), set).ok);   // no number
    }

    return check::summary();
}
