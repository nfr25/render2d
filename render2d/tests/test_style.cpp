// SPDX-License-Identifier: AGPL-3.0-or-later
// YSLD reading, scale rules and filters, with the numbers of notions §23, §24 and §28.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

#include "check.hpp"
#include "style/ysld_reader.hpp"

using namespace r2d;

namespace {

// notions §23: the buildings style that fixes the regional map.
const char* kBuildings = R"(
name: batiments
feature-styles:
- rules:
  - name: proche
    scale: [min, 15000]
    symbolizers:
    - polygon:
        fill-color: '#C98B6B'
        stroke-color: '#7A4A35'
        stroke-width: 0.8
  - name: loin
    scale: [15000, max]
    symbolizers:
    - polygon:
        fill-color: '#8E7B6B'
)";

bool hasWarning(const YsldResult& r, const std::string& part) {
    for (const auto& w : r.warnings) if (w.find(part) != std::string::npos) return true;
    return false;
}

std::string onlyRuleName(const FeatureTypeStyle& fts, double scale) {
    const auto rules = activeRules(fts, scale);
    return rules.size() == 1 ? rules[0]->name : "?";
}

void testBuildings() {
    const YsldResult r = parseYsld(kBuildings);
    CHECK(r.style.has_value());
    CHECK(r.warnings.empty());
    if (!r.style) return;
    const Style& s = *r.style;
    CHECK(s.name == "batiments");
    CHECK(s.featureStyles.size() == 1);
    CHECK(s.featureStyles[0].rules.size() == 2);

    const Rule& near = s.featureStyles[0].rules[0];
    CHECK(near.scale.min == 0.0);
    CHECK(near.scale.max == 15000.0);
    const auto* poly = std::get_if<PolygonSymbolizer>(&near.symbolizers[0]);
    CHECK(poly && poly->fill && poly->stroke);
    if (poly && poly->stroke) CHECK_NEAR(poly->stroke->width, 0.8, 1e-12);
    if (poly && poly->fill)   CHECK_NEAR(poly->fill->color.r, 0xC9 / 255.0, 1e-12);

    const Rule& far = s.featureStyles[0].rules[1];
    CHECK(std::isinf(far.scale.max));
    const auto* farPoly = std::get_if<PolygonSymbolizer>(&far.symbolizers[0]);
    CHECK(farPoly && farPoly->fill && !farPoly->stroke);   // no stroke-color: no outline
}

// notions §24: the three maps.
void testScaleChoice() {
    const Style s = *parseYsld(kBuildings).style;
    const FeatureTypeStyle& f = s.featureStyles[0];
    CHECK(onlyRuleName(f, 16.529 / 0.00028) == "loin");   // whole region, ~1:59033
    CHECK(onlyRuleName(f, 2.0 / 0.00028) == "proche");    // 2 km on 1000 px, ~1:7143
    CHECK(onlyRuleName(f, 15000.0) == "loin");            // the exact limit: [15000, max)
    CHECK(onlyRuleName(f, 14999.999) == "proche");
}

// notions §23: the YAML traps give warnings, never a stop.
void testWarnings() {
    const YsldResult r = parseYsld(R"(
name: pieges
rules:
- symbolizers:
  - polygon:
      fill-color: #C98B6B
      stroke-color: '#7A4A35'
      stroke-widht: 2
  - raster:
      opacity: 1.5
  - extrusion:
      height: 3
  - line:
      stroke-linecap: pointy
      stroke-dasharray: '8 4'
)");
    CHECK(r.style.has_value());
    CHECK(hasWarning(r, "must be quoted"));           // unquoted colour
    CHECK(hasWarning(r, "'stroke-widht'"));           // typo, reported by name...
    CHECK(hasWarning(r, "line 8:"));                  // ...and by line
    CHECK(hasWarning(r, "'opacity' goes from 0 to 1"));   // §66
    CHECK(hasWarning(r, "'extrusion' is not supported"));
    CHECK(hasWarning(r, "stroke-linecap"));
    if (!r.style) return;
    // Root 'rules' shorthand: one feature-style.
    CHECK(r.style->featureStyles.size() == 1);
    const Rule& rule = r.style->featureStyles[0].rules[0];
    CHECK(rule.symbolizers.size() == 3);              // polygon, raster, line; extrusion skipped
    const auto* raster = std::get_if<RasterSymbolizer>(&rule.symbolizers[1]);
    CHECK(raster && raster->opacity == 1.0);          // out of range: 1
    const auto* poly = std::get_if<PolygonSymbolizer>(&rule.symbolizers[0]);
    CHECK(poly && !poly->fill && poly->stroke);       // fill lost to the comment, outline kept
    const auto* line = std::get_if<LineSymbolizer>(&rule.symbolizers[2]);
    CHECK(line && line->stroke.dashes.size() == 2);
    if (line) CHECK(line->stroke.color.r == 0.0);     // SLD default line colour: black
}

void testPointAndOpacity() {
    const YsldResult r = parseYsld(R"(
symbolizers:
- point:
    size: 10
    symbols:
    - mark: {shape: circle, fill-color: '#FF0000', fill-opacity: 0.5, stroke-width: 2}
)");
    CHECK(r.style.has_value());
    if (!r.style) return;
    const auto* pt = std::get_if<PointSymbolizer>(&r.style->featureStyles[0].rules[0].symbolizers[0]);
    CHECK(pt != nullptr);
    if (!pt) return;
    CHECK_NEAR(pt->size, 10.0, 0.0);
    CHECK(pt->fill && pt->stroke);
    if (pt->fill)   CHECK_NEAR(pt->fill->color.a, 0.5, 1e-12);
    if (pt->stroke) CHECK_NEAR(pt->stroke->width, 2.0, 0.0);
}

void testErrors() {
    CHECK(!parseYsld("name: [unclosed").style.has_value());
    CHECK(!parseYsld("- just\n- a list\n").style.has_value());
    CHECK(!readYsldFile("no_such_style.yaml").style.has_value());

    const YsldResult bad = parseYsld("rules:\n- scale: [20000, 10000]\n  symbolizers: []\n");
    CHECK(hasWarning(bad, "first bound must be smaller"));
    CHECK(hasWarning(bad, "nothing will be drawn"));
}

// notions §28: filter, else, and an unreadable filter.
void testFilters() {
    const YsldResult r = parseYsld(R"(
name: adresses
rules:
- name: portes
  filter: ${PARENTID IS NULL}
  symbolizers: [{point: {size: 5}}]
- name: evere
  filter: "${MUNNISCODE = '21006'}"
  symbolizers: [{point: {size: 5}}]
- name: autres
  else: true
  symbolizers: [{point: {size: 3}}]
- name: casse
  filter: ${AREA << 30}
  symbolizers: [{point: {size: 3}}]
)");
    CHECK(r.style.has_value());
    if (!r.style) return;
    const auto& rules = r.style->featureStyles[0].rules;
    CHECK(rules.size() == 4);
    if (rules.size() != 4) return;
    CHECK(rules[0].filter && rules[0].filter->dump() == "(IS NULL PARENTID)");
    CHECK(rules[1].filter && rules[1].filter->dump() == "(= MUNNISCODE '21006')");
    CHECK(!rules[2].filter && rules[2].isElse);
    CHECK(rules[3].invalid);
    CHECK(hasWarning(r, "line 14:"));
    CHECK(hasWarning(r, "column 7"));           // AREA << 30: the second '<'
    CHECK(hasWarning(r, "never applied"));
    CHECK(activeRules(r.style->featureStyles[0], 5000.0).size() == 3);   // the broken rule is out
}

// notions §35: the text symbolizer.
void testText() {
    const YsldResult r = parseYsld(R"(
rules:
- filter: ${PARENTID IS NULL}
  scale: [min, 1100]
  symbolizers:
  - text:
      label: ${POLICENUM}
      font-family: Arial
      font-size: 9
      font-weight: bold
      fill-color: '#3A2A20'
      halo: {radius: 1.5, fill-color: '#FFFFFF'}
      anchor: [0.5, 0.5]
      rotation: ${360 - CARTOANGLE}
  - text:
      label: Evere
      rotation: ${360 -}
      colour: red
)");
    CHECK(r.style.has_value());
    if (!r.style) return;
    const auto& syms = r.style->featureStyles[0].rules[0].symbolizers;
    CHECK(syms.size() == 2);
    if (syms.size() != 2) return;
    const auto* t = std::get_if<TextSymbolizer>(&syms[0]);
    CHECK(t != nullptr);
    if (!t) return;
    CHECK(t->label && t->label->dump() == "POLICENUM");
    CHECK(t->rotation && t->rotation->dump() == "(- 360 CARTOANGLE)");
    CHECK(t->font.family == "Arial" && t->font.size == 9.0 && t->font.bold);
    CHECK(t->halo && t->halo->radius == 1.5);
    CHECK(t->anchorX == 0.5 && t->anchorY == 0.5);
    const auto* t2 = std::get_if<TextSymbolizer>(&syms[1]);
    CHECK(t2 && t2->label && t2->label->dump() == "'Evere'");   // a fixed text
    CHECK(t2 && !t2->rotation);                                  // unreadable: ignored
    CHECK(hasWarning(r, "'rotation'"));
    CHECK(hasWarning(r, "'colour'"));
}

// notions §40 to §43: shapes, size and rotation expressions, images, hatching.
void testSymbols() {
    const std::string dir = std::filesystem::temp_directory_path().string();
    { std::ofstream(dir + "/r2d_icon.png") << "x"; }   // exists: enough for the reader
    const YsldResult r = parseYsld(R"(
rules:
- symbolizers:
  - point:
      size: ${4 * sqrt(N)}
      rotation: ${90 - ANGLE}
      symbols:
      - mark: {shape: triangle, fill-color: '#2F7D4F'}
  - point:
      size: 8
      uom: metre
      symbols:
      - external: {url: r2d_icon.png, format: image/png}
      - mark: {shape: star}
  - point:
      symbols:
      - external: {url: no_such_icon.png, format: image/png}
      - mark: {shape: hexagon}
  - polygon:
      fill-color: '#F2EBDD'
      fill-graphic:
        size: 10
        symbols:
        - mark: {shape: 'shape://slash', stroke-color: '#7A4A35', stroke-width: 1}
)", dir);
    CHECK(r.style.has_value());
    if (!r.style) return;
    const auto& syms = r.style->featureStyles[0].rules[0].symbolizers;
    CHECK(syms.size() == 4);
    if (syms.size() != 4) return;
    const auto& a = std::get<PointSymbolizer>(syms[0]);
    CHECK(a.shape == MarkShape::Triangle);
    CHECK(a.sizeExpr && a.sizeExpr->dump() == "(* 4 (SQRT N))");
    CHECK(a.rotation && a.rotation->dump() == "(- 90 ANGLE)");
    const auto& b = std::get<PointSymbolizer>(syms[1]);
    CHECK(b.metres && b.size == 8.0);
    CHECK(b.image.find("r2d_icon.png") != std::string::npos);
    CHECK(b.shape == MarkShape::Star);                      // the fallback
    const auto& c = std::get<PointSymbolizer>(syms[2]);
    CHECK(c.image.empty());                                 // not found: the mark is drawn
    CHECK(c.shape == MarkShape::Circle);                    // unknown shape: a circle
    CHECK(hasWarning(r, "image not found"));
    CHECK(hasWarning(r, "unknown shape 'hexagon'"));
    const auto& d = std::get<PolygonSymbolizer>(syms[3]);
    CHECK(d.fill && d.graphicFill);
    if (d.graphicFill) {
        CHECK(d.graphicFill->size == 10.0 && d.graphicFill->shape == MarkShape::Slash);
        CHECK(d.graphicFill->stroke && d.graphicFill->stroke->width == 1.0);
    }
    std::filesystem::remove(dir + "/r2d_icon.png");
}

} // namespace

int main() {
    testBuildings();
    testScaleChoice();
    testWarnings();
    testPointAndOpacity();
    testErrors();
    testFilters();
    testText();
    testSymbols();
    return check::summary();
}
