// SPDX-License-Identifier: AGPL-3.0-or-later
// The style catalogue (notions §60). For every kind of symbolizer and every
// property of the catalogue: add, write, read back -> present; remove,
// write, read back -> absent. A property the writer does not know fails here.

#include <cstdio>
#include <string>

#include "check.hpp"
#include "style/catalog.hpp"
#include "style/ysld_reader.hpp"
#include "style/ysld_writer.hpp"

using namespace r2d;

namespace {

Style wrap(const Symbolizer& sym) {
    Rule rule;
    rule.name = "r";
    rule.symbolizers.push_back(sym);
    Style s;
    s.name = "t";
    s.featureStyles.push_back(FeatureTypeStyle{{rule}});
    return s;
}

// Write, read back, return the symbolizer (nothing if lost).
std::optional<Symbolizer> roundTrip(const Symbolizer& sym, std::string& text) {
    text = writeYsld(wrap(sym));
    const YsldResult r = parseYsld(text);
    if (!r.style || r.style->featureStyles.empty() || r.style->featureStyles[0].rules.empty() ||
        r.style->featureStyles[0].rules[0].symbolizers.empty())
        return std::nullopt;
    return r.style->featureStyles[0].rules[0].symbolizers[0];
}

Symbolizer start(SymbolizerKind kind) {
    Symbolizer s = newSymbolizer(kind, 1);
    if (auto* t = std::get_if<TextSymbolizer>(&s)) t->label = Expression::literalText("A");   // a text needs one
    return s;
}

} // namespace

int main() {
    for (SymbolizerKind kind : {SymbolizerKind::Polygon, SymbolizerKind::Line, SymbolizerKind::Point,
                                SymbolizerKind::Text, SymbolizerKind::Raster}) {
        CHECK(kindOf(start(kind)) == kind);
        for (const PropertySpec& p : optionalProperties(kind)) {
            Symbolizer s = start(kind);
            if (!canAddProperty(s, p.key)) {
                // Only allowed when it is already there (the default look has it).
                CHECK(hasProperty(s, p.key));
                removeProperty(s, p.key);
            }
            if (p.key == "dashes" && kind == SymbolizerKind::Polygon && !hasProperty(s, "stroke"))
                addProperty(s, "stroke");
            CHECK(canAddProperty(s, p.key));
            addProperty(s, p.key);
            std::string text;
            auto back = roundTrip(s, text);
            const bool there = back && kindOf(*back) == kind && hasProperty(*back, p.key);
            if (!there) std::printf("  %s + %s lost:\n%s\n", kindName(kind), p.key.c_str(), text.c_str());
            CHECK(there);

            removeProperty(s, p.key);
            back = roundTrip(s, text);
            const bool gone = back && !hasProperty(*back, p.key);
            if (!gone) std::printf("  %s - %s still there:\n%s\n", kindName(kind), p.key.c_str(), text.c_str());
            CHECK(gone);
        }
    }

    // §60's example: a polygon without outline receives one, written as stroke-color alone.
    PolygonSymbolizer bare{.fill = FillStyle{.color = Color::fromHex(0xC98B6B)}};
    Symbolizer s = bare;
    CHECK(!hasProperty(s, "stroke"));
    CHECK(!canAddProperty(s, "dashes"));      // no stroke, no dashes
    addProperty(s, "stroke");
    CHECK(hasProperty(s, "stroke"));
    const std::string text = writeYsld(wrap(s));
    CHECK(text.find("stroke-color: '#333333'") != std::string::npos);
    CHECK(text.find("stroke-width") == std::string::npos);   // 1 is the default

    // Removing the stroke takes its dashes along.
    addProperty(s, "dashes");
    CHECK(hasProperty(s, "dashes"));
    removeProperty(s, "stroke");
    CHECK(!hasProperty(s, "dashes"));

    // Unknown keys and a property already there: nothing happens.
    Symbolizer line = newSymbolizer(SymbolizerKind::Line, 0);
    CHECK(!canAddProperty(line, "halo"));
    addProperty(line, "halo");
    CHECK(!hasProperty(line, "halo"));

    // The colour number turns: 5 looks like 1.
    auto* a = std::get_if<PolygonSymbolizer>(&(s = newSymbolizer(SymbolizerKind::Polygon, 5)));
    Symbolizer one = newSymbolizer(SymbolizerKind::Polygon, 1);
    CHECK(a && a->fill && std::get<PolygonSymbolizer>(one).fill &&
          a->fill->color.r == std::get<PolygonSymbolizer>(one).fill->color.r);

    return check::summary();
}
