// SPDX-License-Identifier: AGPL-3.0-or-later
// Symbolizers, passes, filters and labels on real pixels (notions §19 to §37).
// World = image here: a 100 x 100 m area on 100 x 100 px, 1 m per pixel, Y flipped.

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <string>
#include <vector>

#include "cairo_image.hpp"
#include <cairo.h>

#include "canvas/shapes.hpp"
#include "check.hpp"
#include "core/view.hpp"
#include "render/labels.hpp"
#include "render/render_layer.hpp"
#include "render/symbolize.hpp"

using namespace r2d;

namespace {

const View kView = *makeView({0.0, 0.0, 100.0, 100.0}, {100, 100});   // 1 m/px, ~1:3571

const Color kBlack = Color::fromHex(0x000000);

// An in-memory FeatureSource: lets us test passes and filters without GDAL.
class ListSource final : public FeatureSource {
public:
    explicit ListSource(std::vector<Geometry> g) : geoms_(std::move(g)) {}
    ListSource(std::vector<Geometry> g, std::vector<FieldDef> f, std::vector<Attributes> rows)
        : geoms_(std::move(g)), fields_(std::move(f)), rows_(std::move(rows)) {}

    const std::vector<FieldDef>& fields() const override { return fields_; }
    void setNeededFields(const std::vector<int>& c) override { if (!c.empty()) needed = c; }
    void setSpatialFilter(const Envelope&) override {}
    ReadStats forEach(const FeatureVisitor& visit) override {
        ReadStats s;
        const Attributes none(fields_.size());
        for (std::size_t i = 0; i < geoms_.size(); ++i) {
            ++s.read; ++s.drawn;
            visit(geoms_[i], i < rows_.size() ? rows_[i] : none, static_cast<FeatureId>(i));
        }
        ++reads;
        return s;
    }
    int              reads = 0;
    std::vector<int> needed;          // last non-empty request
private:
    std::vector<Geometry>   geoms_;
    std::vector<FieldDef>   fields_;
    std::vector<Attributes> rows_;
};

Geometry square(double x0, double y0, double x1, double y1) {
    Geometry g;
    g.type = GeomType::Polygon;
    g.points = {{x0, y0}, {x0, y1}, {x1, y1}, {x1, y0}, {x0, y0}};
    g.parts  = {{0, 5}};
    return g;
}

Geometry segment(double x0, double y0, double x1, double y1) {
    Geometry g;
    g.type   = GeomType::Line;
    g.points = {{x0, y0}, {x1, y1}};
    g.parts  = {{0, 2}};
    return g;
}

PolygonSymbolizer blackPolygon() {
    return PolygonSymbolizer{.fill = FillStyle{.color = kBlack}, .stroke = StrokeStyle{.color = kBlack, .width = 1.0}};
}

// §20: k = 4/3 (sqrt 2 - 1).
void testKappa() {
    CHECK_NEAR(kCircleKappa, 0.5522847498, 1e-9);
}

// §16, §19: two rings in one path, even-odd -> the courtyard stays white.
void testPolygonWithHole() {
    TestImage img(100, 100);
    Geometry g = square(10, 10, 90, 90);
    // Courtyard turning the same way as the outer ring (dirty data on purpose).
    g.points.insert(g.points.end(), {{40, 40}, {40, 60}, {60, 60}, {60, 40}, {40, 40}});
    g.parts.push_back({5, 5});
    drawSymbolizer(img.canvas, kView.worldToImage, g, blackPolygon());
    CHECK(img.red(20, 50) == 0);     // built part
    CHECK(img.red(50, 50) == 255);   // courtyard
    CHECK(img.red(5, 5) == 255);     // outside
}

// §16: the gap between two parts of a MultiLineString must stay empty.
void testMultiLineGap() {
    TestImage img(100, 100);
    Geometry g;
    g.type   = GeomType::Line;
    g.points = {{10, 10}, {40, 10}, {60, 10}, {90, 10}};
    g.parts  = {{0, 2}, {2, 2}};
    drawSymbolizer(img.canvas, kView.worldToImage, g, LineSymbolizer{StrokeStyle{.color = kBlack, .width = 3.0}});
    // World y = 10 -> image row 100 - 10 = 90.
    CHECK(img.red(25, 90) == 0);
    CHECK(img.red(75, 90) == 0);
    CHECK(img.red(50, 90) == 255);   // the gap
}

// §19: the point size is a diameter in pixels.
void testPointCircle() {
    TestImage img(100, 100);
    Geometry g;
    g.type   = GeomType::Point;
    g.points = {{50.0, 50.0}};
    g.parts  = {{0, 1}};
    drawSymbolizer(img.canvas, kView.worldToImage, g,
                   PointSymbolizer{.size = 10.0, .fill = FillStyle{.color = kBlack}, .stroke = StrokeStyle{.color = kBlack, .width = 1.0}});
    CHECK(img.red(50, 50) == 0);     // centre
    CHECK(img.red(53, 50) == 0);     // inside the 5 px radius
    CHECK(img.red(58, 50) == 255);   // beyond 5 px + half stroke
    CHECK(img.red(54, 54) == 255);   // diagonal: pixel centre at 4.5*sqrt(2) ~ 6.4 px
}

// SLD: a line symbolizer on a polygon draws its outline, not its inside.
void testLineOnPolygon() {
    TestImage img(100, 100);
    drawSymbolizer(img.canvas, kView.worldToImage, square(20, 20, 80, 80),
                   LineSymbolizer{StrokeStyle{.color = kBlack, .width = 2.0}});
    CHECK(img.red(20, 50) == 0);     // on the outline (x = 20 m -> column 20)
    CHECK(img.red(50, 50) == 255);   // inside: not filled
}

void testReach() {
    CHECK_NEAR(symbolizerReachPx(LineSymbolizer{StrokeStyle{.width = 7.0}}), 3.5, 1e-12);
    CHECK_NEAR(symbolizerReachPx(PointSymbolizer{.size = 10.0, .fill = std::nullopt, .stroke = StrokeStyle{.width = 1.0}}), 5.5, 1e-12);
    CHECK_NEAR(symbolizerReachPx(PolygonSymbolizer{FillStyle{}, std::nullopt}), 0.0, 1e-12);
}

// §24: a rule outside the scale is not drawn; a pass with no active rule is not even read.
void testScaleSkipsPass() {
    Rule farOnly;
    farOnly.scale = ScaleRange{15000.0, ScaleRange{}.max};   // kView is ~1:3571: not active
    farOnly.symbolizers.push_back(blackPolygon());
    Style s;
    s.featureStyles.push_back(FeatureTypeStyle{{farOnly}});

    ListSource src({square(10, 10, 90, 90)});
    TestImage img(100, 100);
    const LayerRenderStats st = renderLayer(src, img.canvas, kView, {&s, &s, &s});
    CHECK(st.passes == 0);
    CHECK(src.reads == 0);
    CHECK(img.red(50, 50) == 255);
}

// §25: roads with a casing. Two passes: all casings, then all centres.
// At the crossing, the white centre must win over the grey casing.
void testRoadCasingPasses() {
    Rule casing, centre;
    // Widths 8 and 4: the casing covers columns 46..53 of a vertical road at
    // x = 50, the centre only 48..51; whole pixels, no anti-aliasing to worry about.
    casing.symbolizers.push_back(LineSymbolizer{StrokeStyle{.color = Color::fromHex(0x555555), .width = 8.0}});
    centre.symbolizers.push_back(LineSymbolizer{StrokeStyle{.color = Color::fromHex(0xFFFFFF), .width = 4.0}});

    // Two roads crossing at (50, 50). Background: black, to see the white centre.
    const std::vector<Geometry> roads = {segment(10, 50, 90, 50), segment(50, 10, 50, 90)};

    {   // Two feature-styles: clean crossing.
        Style s;
        s.featureStyles.push_back(FeatureTypeStyle{{casing}});
        s.featureStyles.push_back(FeatureTypeStyle{{centre}});
        ListSource src(roads);
        TestImage img(100, 100);
        img.canvas.clear(kBlack);
        const LayerRenderStats st = renderLayer(src, img.canvas, kView, {&s, &s, &s});
        CHECK(st.passes == 2);
        CHECK(src.reads == 2);                 // one read per pass
        CHECK(img.red(50, 50) == 255);         // crossing: white
        CHECK(img.red(46, 49) == 255);         // road 1's centre, beside road 2: still white
    }
    {   // Same symbolizers in ONE feature-style: road 2's casing cuts road 1's centre.
        Rule both;
        both.symbolizers = {casing.symbolizers[0], centre.symbolizers[0]};
        Style s;
        s.featureStyles.push_back(FeatureTypeStyle{{both}});
        ListSource src(roads);
        TestImage img(100, 100);
        img.canvas.clear(kBlack);
        renderLayer(src, img.canvas, kView, {&s, &s, &s});
        // The same pixel of road 1's centre is now under road 2's grey casing.
        CHECK(img.red(46, 49) == 0x55);
    }
}

Rule filtered(const std::string& name, const std::string& filter, Color c) {
    Rule r;
    r.name = name;
    std::string error;
    r.filter = Expression::parseCondition(filter, error);
    r.symbolizers.push_back(PolygonSymbolizer{FillStyle{.color = c}, std::nullopt});
    return r;
}

// §28, §31: classes of area, an else rule for the NULL, counters per rule,
// and only the needed column asked for.
void testFiltersAndElse() {
    const std::vector<FieldDef> fields = {{"ID", FieldType::Text}, {"AREA", FieldType::Real}};
    const std::vector<Geometry> g = {square(0, 0, 20, 20), square(30, 0, 50, 20), square(60, 0, 80, 20)};
    const std::vector<Attributes> rows = {
        {std::string("a"), 12.0},              // small
        {std::string("b"), 250.0},             // big
        {std::string("c"), std::monostate{}},  // area NULL
    };
    Rule small = filtered("small", "AREA < 30", Color::fromHex(0xFF0000));
    Rule notSmall = filtered("not small", "NOT (AREA < 30)", Color::fromHex(0x0000FF));
    Rule other;
    other.name = "other";
    other.isElse = true;
    other.symbolizers.push_back(PolygonSymbolizer{FillStyle{.color = Color::fromHex(0x00FF00)}, std::nullopt});

    Style s;
    s.featureStyles.push_back(FeatureTypeStyle{{small, notSmall, other}});
    ListSource src(g, fields, rows);
    TestImage img(100, 100);
    const LayerRenderStats st = renderLayer(src, img.canvas, kView, {&s, &s, &s});

    CHECK(st.filtered);
    CHECK(src.needed.size() == 2 && src.needed[0] == 1);   // AREA twice, never ID
    CHECK(st.fieldsRead.size() == 1 && st.fieldsRead[0] == "AREA");
    CHECK(st.rules.size() == 3);
    if (st.rules.size() == 3) {
        CHECK(st.rules[0].name == "small"     && st.rules[0].drawn == 1);
        CHECK(st.rules[1].name == "not small" && st.rules[1].drawn == 1);
        CHECK(st.rules[2].name == "other"     && st.rules[2].drawn == 1);   // only the NULL
    }
    // World y = 10 -> row 90. Red, blue, green.
    CHECK(img.red(10, 90) == 255 && img.blue(10, 90) == 0);
    CHECK(img.red(40, 90) == 0   && img.blue(40, 90) == 255);
    CHECK(img.red(70, 90) == 0   && img.green(70, 90) == 255 && img.blue(70, 90) == 0);
}

// A filter naming a field the layer does not have: reported, counts as NULL.
void testMissingField() {
    const std::vector<FieldDef> fields = {{"AREA", FieldType::Real}};
    Rule r = filtered("r", "SURFACE > 30", Color::fromHex(0x000000));
    Style s;
    s.featureStyles.push_back(FeatureTypeStyle{{r}});
    ListSource src({square(10, 10, 90, 90)}, fields, {{40.0}});
    TestImage img(100, 100);
    const LayerRenderStats st = renderLayer(src, img.canvas, kView, {&s, &s, &s});
    CHECK(st.missingFields.size() == 1 && st.missingFields[0] == "SURFACE");
    CHECK(st.rules.size() == 1 && st.rules[0].drawn == 0);
    CHECK(img.red(50, 50) == 255);
}

// --- labels: notions §35 to §37 --------------------------------------------

// The box of the pixels darker than `limit` (red channel).
struct InkBox { int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; bool any() const { return x1 >= 0; } };
InkBox inkBox(TestImage& img, int w, int h, int limit = 128) {
    InkBox b;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (img.red(x, y) < limit) { b.x0 = std::min(b.x0, x); b.y0 = std::min(b.y0, y);
                                         b.x1 = std::max(b.x1, x); b.y1 = std::max(b.y1, y); }
    return b;
}

Label label(const std::string& text, double size, double rotation = 0.0) {
    Label l;
    l.text = text;
    l.font.family = "DejaVu Sans";   // falls back to any sans font when absent
    l.font.size = size;
    l.fill = kBlack;
    l.at = {50.0, 50.0};
    l.anchorX = 0.5;
    l.anchorY = 0.5;
    l.rotation = rotation;
    return l;
}

void testReadableAngle() {
    CHECK_NEAR(readableAngle(40.68), 40.68, 1e-9);           // rue de Paris 3: 360 - 319.32
    CHECK_NEAR(readableAngle(360.0 - 131.76), 48.24, 1e-9);  // avenue du Cimetiere: turned over
    CHECK_NEAR(readableAngle(-30.0), 330.0, 1e-9);
    CHECK_NEAR(readableAngle(270.0), 90.0, 1e-9);
    CHECK_NEAR(readableAngle(90.0), 90.0, 1e-9);
    CHECK_NEAR(readableAngle(720.0 + 10.0), 10.0, 1e-9);
}

void testLabelText() {
    CHECK(labelText(Value{120.0}) == "120");
    CHECK(labelText(Value{120.5}) == "120.5");
    CHECK(labelText(Value{7LL}) == "7");
    CHECK(labelText(Value{std::string("75B")}) == "75B");
    CHECK(labelText(Value{}).empty());
}

// §35: anchor [0.5, 0.5] puts the centre of the ink on the point.
void testLabelCentred() {
    TestImage img(100, 100);
    drawLabels(img.canvas, {label("88", 20.0)});
    const InkBox b = inkBox(img, 100, 100);
    CHECK(b.any());
    CHECK_NEAR((b.x0 + b.x1) / 2.0, 50.0, 1.5);
    CHECK_NEAR((b.y0 + b.y1) / 2.0, 50.0, 1.5);
}

// §36: rotated by 90 degrees, a wide text becomes tall.
void testLabelRotated() {
    TestImage flat(100, 100), turned(100, 100);
    drawLabels(flat.canvas, {label("1111", 16.0)});
    drawLabels(turned.canvas, {label("1111", 16.0, 90.0)});
    const InkBox a = inkBox(flat, 100, 100), b = inkBox(turned, 100, 100);
    CHECK(a.x1 - a.x0 > a.y1 - a.y0);
    CHECK(b.y1 - b.y0 > b.x1 - b.x0);
    CHECK_NEAR((b.y0 + b.y1) / 2.0, 50.0, 1.5);   // still centred
}

// §35: the halo whitens the pixels around the glyphs, not the background further away.
void testHalo() {
    TestImage img(100, 100);
    img.canvas.clear(Color::fromHex(0x808080));
    Label l = label("8", 40.0);
    l.halo = Halo{3.0, Color::fromHex(0xFFFFFF)};
    drawLabels(img.canvas, {l});
    int white = 0, black = 0;
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 100; ++x) { if (img.red(x, y) > 240) ++white; if (img.red(x, y) < 20) ++black; }
    CHECK(white > 50);
    CHECK(black > 50);
    CHECK(img.red(2, 2) == 0x80);
}

// §35: renderLayer collects labels (NULL text counted apart), rotation from an expression.
void testLayerLabels() {
    const std::vector<FieldDef> fields = {{"POLICENUM", FieldType::Text}, {"CARTOANGLE", FieldType::Real}};
    Geometry p1, p2;
    p1.type = p2.type = GeomType::Point;
    p1.points = {{30.0, 50.0}}; p1.parts = {{0, 1}};
    p2.points = {{70.0, 50.0}}; p2.parts = {{0, 1}};
    const std::vector<Attributes> rows = {{std::string("106"), 319.32}, {std::monostate{}, 0.0}};

    std::string e;
    TextSymbolizer t;
    t.label = Expression::parseValue("POLICENUM", e);
    t.rotation = Expression::parseValue("360 - CARTOANGLE", e);
    Rule r;
    r.name = "numeros";
    r.symbolizers.push_back(t);
    Style s;
    s.featureStyles.push_back(FeatureTypeStyle{{r}});

    ListSource src({p1, p2}, fields, rows);
    TestImage img(100, 100);
    std::vector<Label> labels;
    const LayerRenderStats st = renderLayer(src, img.canvas, kView, {&s, &s, &s}, &labels);
    CHECK(st.labels == 1);
    CHECK(st.labelsEmpty == 1);                 // the NULL number
    CHECK(src.needed.size() == 2);              // both columns read
    CHECK(labels.size() == 1);
    if (labels.size() == 1) {
        CHECK(labels[0].text == "106");
        CHECK_NEAR(labels[0].rotation, 40.68, 1e-9);
        CHECK_NEAR(labels[0].at.x, 30.0, 1e-9);   // world (30, 50) -> image (30, 50)
    }
    CHECK(img.red(30, 50) == 255);              // collected, not drawn yet
}

// §39: pixel ratio 1.5, every length in pixels times 1.5, scale for the pixels seen.
void testPixelRatio() {
    Rule r;
    TextSymbolizer t;
    t.font.size = 9.0;
    t.halo = Halo{1.5, Color{}};
    r.symbolizers.push_back(t);
    r.symbolizers.push_back(PolygonSymbolizer{std::nullopt, StrokeStyle{.width = 0.8, .dashes = {8.0, 4.0}}});
    r.symbolizers.push_back(PointSymbolizer{.size = 5.0, .fill = std::nullopt, .stroke = StrokeStyle{.width = 1.0}});
    Style s;
    s.featureStyles.push_back(FeatureTypeStyle{{r}});
    const Style k = scaledStyle(s, 1.5);
    const auto& syms = k.featureStyles[0].rules[0].symbolizers;
    const auto& tt = std::get<TextSymbolizer>(syms[0]);
    CHECK_NEAR(tt.font.size, 13.5, 1e-12);
    CHECK_NEAR(tt.halo->radius, 2.25, 1e-12);
    const auto& pp = std::get<PolygonSymbolizer>(syms[1]);
    CHECK_NEAR(pp.stroke->width, 1.2, 1e-12);
    CHECK_NEAR(pp.stroke->dashes[0], 12.0, 1e-12);
    const auto& pt = std::get<PointSymbolizer>(syms[2]);
    CHECK_NEAR(pt.size, 7.5, 1e-12);
    CHECK_NEAR(std::get<TextSymbolizer>(s.featureStyles[0].rules[0].symbolizers[0]).font.size, 9.0, 0.0);  // copy

    // The capture of notions §39: 3.847 m per image pixel, seen 1.5 times smaller.
    View v = *makeView({0.0, 0.0, 3847.0, 1000.0}, {1000, 260});
    CHECK_NEAR(v.scaleDenominator(), 3.847 / 0.00028, 1.0);       // ~ 1:13 739
    v.pixelRatio = 1.5;
    CHECK_NEAR(v.scaleDenominator(), 3.847 * 1.5 / 0.00028, 1.0); // ~ 1:20 609
}

// --- symbols: notions §40 to §43 ---------------------------------------------

Geometry pointAt(double x, double y) {
    Geometry g;
    g.type = GeomType::Point;
    g.points = {{x, y}};
    g.parts = {{0, 1}};
    return g;
}

PointSymbolizer mark(MarkShape shape, double size) {
    return PointSymbolizer{.size = size, .fill = FillStyle{.color = kBlack}, .stroke = std::nullopt, .shape = shape};
}

// §40: a triangle of 20 pixels: top vertex 10 px above the point, base 5 px below.
void testMarks() {
    {
        TestImage img(100, 100);   // world (50, 50) -> image (50, 50)
        drawSymbolizer(img.canvas, kView.worldToImage, pointAt(50, 50), mark(MarkShape::Triangle, 20.0));
        CHECK(img.red(50, 42) == 0);     // inside, near the top vertex (y = 40)
        CHECK(img.red(50, 38) == 255);   // above the vertex
        CHECK(img.red(50, 53) == 0);     // just above the base: 50 + 10 * 0.5 = 55
        CHECK(img.red(50, 57) == 255);   // below the base
    }
    {
        TestImage img(100, 100);
        drawSymbolizer(img.canvas, kView.worldToImage, pointAt(50, 50), mark(MarkShape::Square, 20.0));
        CHECK(img.red(41, 41) == 0 && img.red(58, 58) == 0);   // corners of 40..60
        CHECK(img.red(38, 50) == 255 && img.red(61, 50) == 255);
    }
    {   // The cross fills its centre once, the x leaves the top middle empty.
        TestImage cross(100, 100), ex(100, 100);
        drawSymbolizer(cross.canvas, kView.worldToImage, pointAt(50, 50), mark(MarkShape::Cross, 20.0));
        drawSymbolizer(ex.canvas, kView.worldToImage, pointAt(50, 50), mark(MarkShape::X, 20.0));
        CHECK(cross.red(50, 50) == 0 && ex.red(50, 50) == 0);
        CHECK(cross.red(50, 42) == 0);    // the vertical bar
        CHECK(ex.red(50, 42) == 255);     // between two branches of the x
        CHECK(ex.red(44, 44) == 0);       // on a diagonal branch
    }
    {   // §41: rotated by 180 degrees, the triangle points down.
        TestImage img(100, 100);
        SymbolContext ctx;
        ctx.rotation = 180.0;
        drawSymbolizer(img.canvas, kView.worldToImage, pointAt(50, 50), mark(MarkShape::Triangle, 20.0), ctx);
        CHECK(img.red(50, 56) == 0);      // near the vertex, now at y = 60
        CHECK(img.red(50, 43) == 255);
    }
}

// §41: size from an expression (4 * sqrt(4) = 8 px) and in metres (8 m at 1 m/px).
void testSymbolSizes() {
    const std::vector<FieldDef> fields = {{"N", FieldType::Integer}};
    std::string e;
    PointSymbolizer p = mark(MarkShape::Square, 2.0);
    p.sizeExpr = Expression::parseValue("4 * sqrt(N)", e);
    Rule r;
    r.symbolizers.push_back(p);
    Style s;
    s.featureStyles.push_back(FeatureTypeStyle{{r}});
    ListSource src({pointAt(50, 50)}, fields, {{4LL}});
    TestImage img(100, 100);
    renderLayer(src, img.canvas, kView, {&s, &s, &s});
    CHECK(img.red(47, 47) == 0 && img.red(52, 52) == 0);    // square 46..54
    CHECK(img.red(44, 50) == 255 && img.red(55, 50) == 255);

    TestImage img2(100, 100);
    PointSymbolizer m = mark(MarkShape::Square, 8.0);
    m.metres = true;
    drawSymbolizer(img2.canvas, kView.worldToImage, pointAt(50, 50), m);
    CHECK(img2.red(47, 47) == 0 && img2.red(44, 50) == 255);   // 8 m = 8 px here
    const Style k = scaledStyle(Style{"", {FeatureTypeStyle{{Rule{"", {}, {}, false, false, {m}}}}}}, 1.5);
    CHECK_NEAR(std::get<PointSymbolizer>(k.featureStyles[0].rules[0].symbolizers[0]).size, 8.0, 0.0);  // metres: not scaled
}

// §42: a PNG drawn at its size; a missing one gives the mark and an error.
void testImage() {
    const std::string path = (std::filesystem::temp_directory_path() / "r2d_red.png").string();
    {
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 2);
        cairo_t* cr = cairo_create(s);
        cairo_set_source_rgb(cr, 1, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_write_to_png(s, path.c_str());
        cairo_surface_destroy(s);
    }
    PointSymbolizer p = mark(MarkShape::Circle, 8.0);   // 8 px high -> 16 x 8
    p.image = path;
    TestImage img(100, 100);
    drawSymbolizer(img.canvas, kView.worldToImage, pointAt(50, 50), p);
    // Scaled 4 times with a smooth filter: the edges fade over about 2 pixels.
    CHECK(img.red(50, 50) == 255 && img.green(50, 50) < 10);   // red at the centre
    CHECK(img.green(46, 50) < 10 && img.green(54, 50) < 10);   // still red 4 px aside (42..58)
    CHECK(img.green(50, 42) == 255 && img.green(39, 50) == 255);   // white beyond (46..54, 42..58)

    p.image = path + ".missing";
    TestImage img2(100, 100);
    std::vector<std::string> errors;
    SymbolContext ctx;
    ctx.errors = &errors;
    drawSymbolizer(img2.canvas, kView.worldToImage, pointAt(50, 50), p, ctx);
    CHECK(errors.size() == 1);
    CHECK(img2.red(50, 50) == 0);                               // the black circle instead
    std::filesystem::remove(path);
}

// §43: hatching anchored to the world: two images side by side give exactly
// the pixels of one image twice as wide, even with a 10-pixel tile.
void testHatchSeam() {
    PolygonSymbolizer h;
    h.graphicFill = GraphicFill{.size = 10.0, .shape = MarkShape::Slash,
                                .stroke = StrokeStyle{.color = kBlack, .width = 1.0}};
    // Real Lambert 72 coordinates: the world origin is 152 000 px away, far beyond
    // what Cairo's pixman holds (16.16 fixed point). The first version drew nothing here.
    const double X = 152000.0, Y = 173000.0;
    const Geometry big = square(X - 10, Y - 10, X + 210, Y + 110);
    const View wide = *makeView({X, Y, X + 200.0, Y + 100.0}, {200, 100});
    const View left = *makeView({X, Y, X + 100.0, Y + 100.0}, {100, 100});
    const View right = *makeView({X + 100.0, Y, X + 200.0, Y + 100.0}, {100, 100});
    TestImage w(200, 100), a(100, 100), b(100, 100);
    drawSymbolizer(w.canvas, wide.worldToImage, big, h);
    drawSymbolizer(a.canvas, left.worldToImage, big, h);
    drawSymbolizer(b.canvas, right.worldToImage, big, h);
    int diff = 0, dark = 0;
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 100; ++x) {
            if (a.red(x, y) != w.red(x, y)) ++diff;
            if (b.red(x, y) != w.red(x + 100, y)) ++diff;
            if (a.red(x, y) < 128) ++dark;
        }
    CHECK(diff == 0);
    CHECK(dark > 500 && dark < 3000);   // hatched, not filled: about 1 pixel in 10
}

} // namespace

int main() {
    testKappa();
    testPolygonWithHole();
    testMultiLineGap();
    testPointCircle();
    testLineOnPolygon();
    testReach();
    testScaleSkipsPass();
    testRoadCasingPasses();
    testFiltersAndElse();
    testMissingField();
    testReadableAngle();
    testLabelText();
    testLabelCentred();
    testLabelRotated();
    testHalo();
    testLayerLabels();
    testPixelRatio();
    testMarks();
    testSymbolSizes();
    testImage();
    testHatchSeam();
    return check::summary();
}
