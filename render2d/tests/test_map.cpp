// SPDX-License-Identifier: AGPL-3.0-or-later
// The map as a library (notions §45 to §47): opening, warnings, visibility,
// pixel ratio round trip, stopping between layers, report lines.
// Two small GeoJSON layers in Lambert 72 and one style, written by the test.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "backend/cairo/cairo_image.hpp"
#include "check.hpp"
#include "map/map.hpp"

using namespace r2d;

namespace {

// A 60 x 60 m square in the middle of the 100 x 100 m test area.
const char* kSquares = R"({
 "type": "FeatureCollection",
 "crs": {"type": "name", "properties": {"name": "EPSG:31370"}},
 "features": [
  {"type":"Feature","properties":{"id":1},"geometry":{"type":"Polygon","coordinates":[
    [[150020,160020],[150080,160020],[150080,160080],[150020,160080],[150020,160020]]]}}
 ]
})";

// Two points near the corners, far from the middle.
const char* kPoints = R"({
 "type": "FeatureCollection",
 "crs": {"type": "name", "properties": {"name": "EPSG:31370"}},
 "features": [
  {"type":"Feature","properties":{"id":1},"geometry":{"type":"Point","coordinates":[150005,160005]}},
  {"type":"Feature","properties":{"id":2},"geometry":{"type":"Point","coordinates":[150095,160095]}}
 ]
})";

// A red fill with a thick outline (scaled by the pixel ratio), and a filter on
// a field that does not exist: a warning found while RENDERING, not opening.
const char* kStyle = R"(name: test
feature-styles:
- rules:
  - name: red
    symbolizers:
    - polygon:
        fill-color: '#FF0000'
        stroke-color: '#000000'
        stroke-width: 4
  - name: never
    filter: ${NOPE = 1}
    symbolizers:
    - polygon:
        fill-color: '#00FF00'
)";

struct Files {
    std::string squares, points, style, missing;
};

Files writeFiles() {
    const auto dir = std::filesystem::temp_directory_path();
    Files f;
    f.squares = (dir / "r2d_map_squares.geojson").string();
    f.points  = (dir / "r2d_map_points.geojson").string();
    f.style   = (dir / "r2d_map_style.yaml").string();
    f.missing = (dir / "r2d_map_absent.geojson").string();
    std::ofstream(f.squares) << kSquares;
    std::ofstream(f.points) << kPoints;
    std::ofstream(f.style) << kStyle;
    std::filesystem::remove(f.missing);
    return f;
}

const Envelope kArea{150000.0, 160000.0, 150100.0, 160100.0};

View testView() { return *makeView(kArea, ImageSize{100, 100}); }   // 1 m/px

// A white 100 x 100 image with the map drawn on it.
std::unique_ptr<CairoImage> draw(Map& map, MapReport* report = nullptr,
                                 const std::function<bool()>& stop = {}) {
    std::string error;
    auto img = CairoImage::create(ImageSize{100, 100}, error);
    img->canvas().clear(Color::fromHex(0xFFFFFF));
    const MapReport r = map.render(img->canvas(), testView(), stop);
    if (report) *report = r;
    return img;
}

// Red channel of pixel (x, y): byte 2 of B, G, R, A (§48).
int red(CairoImage& img, int x, int y)   { return img.pixels()[y * img.stride() + x * 4 + 2]; }
int green(CairoImage& img, int x, int y) { return img.pixels()[y * img.stride() + x * 4 + 1]; }

bool sameImage(CairoImage& a, CairoImage& b) {
    for (int y = 0; y < a.size().height; ++y)
        if (std::memcmp(a.pixels() + y * a.stride(), b.pixels() + y * b.stride(), a.size().width * 4) != 0)
            return false;
    return true;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// §46: a missing source is a warning; the others open, in order.
void testOpen(const Files& f) {
    Map map = Map::open({{f.squares, f.style}, {f.missing, {}}, {f.points, {}}});
    CHECK(!map.empty());
    CHECK(map.layerCount() == 2);
    CHECK(map.warnings().size() == 1);
    if (!map.warnings().empty()) CHECK(contains(map.warnings()[0], "r2d_map_absent"));
    CHECK(map.layerInfo(0).name == "r2d_map_squares");
    CHECK(map.layerInfo(1).name == "r2d_map_points");
    CHECK(map.crs().epsg == 31370);

    const auto e = map.extent();   // the square and the two points
    CHECK(e.has_value());
    if (e) {
        CHECK_NEAR(e->xmin, 150005.0, 1e-6);
        CHECK_NEAR(e->ymax, 160095.0, 1e-6);
    }
}

void testEmpty(const Files& f) {
    Map map = Map::open({{f.missing, {}}});
    CHECK(map.empty());
    CHECK(map.warnings().size() == 1);
    CHECK(!map.crs().known);
    CHECK(!map.extent().has_value());
}

// The square is red in the middle; the render warning is in the report only.
void testRender(const Files& f) {
    Map map = Map::open({{f.squares, f.style}, {f.points, {}}});
    MapReport r;
    auto img = draw(map, &r);
    CHECK(r.layers.size() == 2);
    if (r.layers.size() == 2) CHECK(r.layers[0].stats.read.drawn == 1);
    CHECK(red(*img, 50, 50) == 255 && green(*img, 50, 50) == 0);
    CHECK(red(*img, 10, 50) == 255 && green(*img, 10, 50) == 255);   // outside: white

    CHECK(map.warnings().empty());
    bool missingField = false;
    for (const auto& w : r.warnings) missingField = missingField || contains(w, "'NOPE'");
    CHECK(missingField);
}

// §46: a hidden layer is not drawn, not reported; the others do not change.
void testHidden(const Files& f) {
    Map map = Map::open({{f.squares, f.style}, {f.points, {}}});
    map.setVisible(0, false);
    CHECK(!map.visible(0));
    MapReport r;
    auto img = draw(map, &r);
    CHECK(r.layers.size() == 1);
    if (r.layers.size() == 1) CHECK(r.layers[0].name == "r2d_map_points");
    CHECK(green(*img, 50, 50) == 255);   // the square is gone
}

// §46: 1.5 then back to 1 gives exactly the map opened at 1.
void testPixelRatioRoundTrip(const Files& f) {
    Map at1 = Map::open({{f.squares, f.style}}, 1.0);
    Map at15 = Map::open({{f.squares, f.style}}, 1.5);
    Map back = Map::open({{f.squares, f.style}}, 1.5);
    back.setPixelRatio(1.0);

    auto i1 = draw(at1), i15 = draw(at15), iBack = draw(back);
    CHECK(!sameImage(*i1, *i15));    // the outline is thicker at 1.5
    CHECK(sameImage(*i1, *iBack));   // no drift
}

// §46: `stop` is asked before each layer; labels are not drawn after a stop.
void testStop(const Files& f) {
    Map map = Map::open({{f.squares, f.style}, {f.points, {}}});
    int calls = 0;
    MapReport r;
    draw(map, &r, [&] { return ++calls > 1; });   // yes to the first layer, no more
    CHECK(calls == 2);
    CHECK(r.stopped);
    CHECK(r.layers.size() == 1);
    const auto lines = reportLines(r);
    CHECK(!lines.empty() && contains(lines.back(), "stopped"));
}

// §47: the lines of render2d; the map's pixel ratio wins over the view's.
void testReportLines(const Files& f) {
    Map map = Map::open({{f.squares, {}}, {f.points, {}}}, 1.5);
    View v = testView();
    v.pixelRatio = 3.0;   // ignored: the styles were scaled for 1.5
    std::string error;
    auto img = CairoImage::create(ImageSize{100, 100}, error);
    const MapReport r = map.render(img->canvas(), v);
    CHECK_NEAR(r.view.pixelRatio, 1.5, 0.0);

    const auto lines = reportLines(r);
    CHECK(lines.size() == 3);   // map: + two layers, no filter, no label
    if (lines.size() == 3) {
        // 1 m/px x 1.5 / 0.00028 = 5357.14 -> 1:5357
        CHECK(lines[0] == "map: EPSG:31370 (BD72 / Belgian Lambert 72), 100x100 px, 1.000 m/px, "
                          "scale 1:5357 (pixel ratio 1.5: 1.500 m per pixel seen)");
        CHECK(contains(lines[1], "r2d_map_squares") && contains(lines[1], "1 read"));
        CHECK(contains(lines[2], "2 read") && contains(lines[2], "1 pass)"));
    }
}

// §50 to §53: the top layer first, hidden layers skipped, FIDs from GDAL.
void testQueryAt(const Files& f) {
    Map map = Map::open({{f.squares, f.style}, {f.points, {}}});
    const View v = testView();   // 1 m/px, 150000..150100 x 160000..160100

    // The middle of the square: pixel (50, 50) is (150050.5, 160049.5).
    auto hits = map.queryAt(v, {50.5, 50.5});
    CHECK(hits.size() == 1);
    if (hits.size() == 1) {
        CHECK(hits[0].layer == 0);
        CHECK(hits[0].geometry.type == GeomType::Polygon);
        CHECK(!hits[0].attributes.empty());   // every field, even those no filter reads
    }

    // The point (150005, 160005) is pixel (5, 95): the points layer, on top, answers.
    hits = map.queryAt(v, {5.5, 95.5});
    CHECK(hits.size() == 1 && hits[0].layer == 1);
    const FeatureId fid = hits.empty() ? -1 : hits[0].fid;
    hits = map.queryAt(v, {5.5, 95.5});
    CHECK(!hits.empty() && hits[0].fid == fid);   // the same FID each time

    // 15 px away from the point: nothing (tolerance 4 px + the default mark).
    CHECK(map.queryAt(v, {20.5, 95.5}).empty());

    // A hidden layer answers nothing: one selects only what one sees (§51).
    map.setVisible(1, false);
    CHECK(map.queryAt(v, {5.5, 95.5}).empty());
    map.setVisible(0, false);
    CHECK(map.queryAt(v, {50.5, 50.5}).empty());
}

// §54, §56: a zone instead of a point.
void testQueryArea(const Files& f) {
    Map map = Map::open({{f.squares, f.style}, {f.points, {}}});
    const View v = testView();

    // A rectangle over the lower left corner: the point (150005, 160005), and
    // the corner of the square (150020, 160020). The points layer, on top, first.
    auto q = map.queryArea(v, {{150000, 160000}, {150030, 160000}, {150030, 160030}, {150000, 160030}});
    CHECK(q.error.empty() && !q.truncated);
    CHECK(q.hits.size() == 2);
    if (q.hits.size() == 2) {
        CHECK(q.hits[0].layer == 1);
        CHECK(q.hits[1].layer == 0);
        CHECK(!q.hits[1].attributes.empty());
    }

    // A small rectangle inside the square (case 3: the zone in the polygon).
    q = map.queryArea(v, {{150040, 160040}, {150050, 160040}, {150050, 160050}, {150040, 160050}});
    CHECK(q.hits.size() == 1 && q.hits[0].layer == 0);

    // The geometry, not the symbol: 3 m from the point, outside, is nothing (a click there would touch it).
    q = map.queryArea(v, {{150008, 160008}, {150015, 160008}, {150015, 160015}, {150008, 160015}});
    CHECK(q.hits.empty());

    // The cap: 1 allowed, 2 found -> truncated, the first one kept.
    q = map.queryArea(v, {{150000, 160000}, {150030, 160000}, {150030, 160030}, {150000, 160030}}, 1);
    CHECK(q.truncated && q.hits.size() == 1 && q.hits[0].layer == 1);
    q = map.queryArea(v, {{150000, 160000}, {150030, 160000}, {150030, 160030}, {150000, 160030}}, 2);
    CHECK(!q.truncated && q.hits.size() == 2);

    // A closed ring is the same ring; fewer than 3 points is refused.
    q = map.queryArea(v, {{150040, 160040}, {150050, 160040}, {150050, 160050}, {150040, 160040}});
    CHECK(q.hits.size() == 1);
    q = map.queryArea(v, {{150040, 160040}, {150050, 160050}, {150040, 160040}});
    CHECK(!q.error.empty() && q.hits.empty());

    // Hidden layers answer nothing (§51).
    map.setVisible(1, false);
    q = map.queryArea(v, {{150000, 160000}, {150030, 160000}, {150030, 160030}, {150000, 160030}});
    CHECK(q.hits.size() == 1 && q.hits[0].layer == 0);
}

// §58: a map that changes.
LayerSpec withColour(const std::string& source, int colour) {
    LayerSpec s(source);
    s.colour = colour;
    return s;
}

void testAddRemoveMove(const Files& f) {
    Map map = Map::open({{f.squares, f.style}});
    std::vector<std::string> warnings;

    // On top by default; a missing source changes nothing.
    CHECK(map.addLayer({f.points}, -1, warnings));
    CHECK(map.layerCount() == 2 && map.layerInfo(1).name == "r2d_map_points");
    CHECK(!map.addLayer({f.missing}, 0, warnings));
    CHECK(map.layerCount() == 2 && !warnings.empty());

    // Order: the red square under a second square in the default style hides it.
    warnings.clear();
    CHECK(map.addLayer({f.squares}, 1, warnings));   // between the red square and the points
    CHECK(map.layerCount() == 3 && map.layerInfo(1).name == "r2d_map_squares");
    CHECK(red(*draw(map), 50, 50) != 255 || green(*draw(map), 50, 50) != 0);   // not the pure red
    map.moveLayer(0, 1);                              // the red one goes above
    auto img = draw(map);
    CHECK(red(*img, 50, 50) == 255 && green(*img, 50, 50) == 0);

    // Removing: back to a map of the added square alone. It arrived third:
    // its default colour is 2, and stays 2 (§58).
    map.removeLayer(1);                               // the red one
    map.removeLayer(1);                               // the points
    Map plain = Map::open({withColour(f.squares, 2)});
    CHECK(sameImage(*draw(map), *draw(plain)));
}

// §58: the default colour follows the layer when it moves.
void testColourFollows(const Files& f) {
    Map map = Map::open({{f.squares}, {f.squares}});     // colours 0 and 1, by position
    map.moveLayer(1, 0);                                 // the colour-1 one goes to the bottom
    Map expected = Map::open({withColour(f.squares, 1), withColour(f.squares, 0)});
    CHECK(sameImage(*draw(map), *draw(expected)));
    Map byPosition = Map::open({{f.squares}, {f.squares}});
    CHECK(!sameImage(*draw(map), *draw(byPosition)));   // the colours did not swap places
}

// §64: selecting by the attributes. Five squares far apart, AREA 10 to 50.
void testQueryWhere() {
    const auto path = (std::filesystem::temp_directory_path() / "r2d_map_where.geojson").string();
    std::ofstream out(path);
    out << R"({"type":"FeatureCollection","features":[)";
    const char* types[] = {"maison", "immeuble", "maison", "annexe", "l'atelier"};
    for (int k = 0; k < 5; ++k) {
        const double x = 150000 + k * 1000;   // far apart: most outside any view
        out << (k ? "," : "") << R"({"type":"Feature","properties":{"AREA":)" << (k + 1) * 10
            << R"(,"TYPE":")" << types[k] << R"("},"geometry":{"type":"Polygon","coordinates":[[[)"
            << x << ",160000],[" << x + 10 << ",160000],[" << x + 10 << ",160010],[" << x << ",160010],["
            << x << ",160000]]]}}";
    }
    out << "]}";
    out.close();

    Map map = Map::open({{path, {}}});
    map.setVisible(0, false);   // hidden: selecting by attributes reads it all the same

    AreaQuery q = map.queryWhere(0, "AREA > 25");
    CHECK(q.error.empty() && q.matched == 3 && q.hits.size() == 3 && !q.truncated);
    if (q.hits.size() == 3) {
        CHECK(q.hits[0].layer == 0);
        CHECK(q.hits[0].attributes.size() == 2);   // every field, not only AREA
        CHECK(std::holds_alternative<std::string>(q.hits[0].attributes[1]));
        CHECK(q.hits[0].geometry.points.size() >= 4);   // the geometry, for the drawing
    }
    q = map.queryWhere(0, "TYPE = 'maison' OR TYPE = 'l''atelier'");
    CHECK(q.matched == 3);
    q = map.queryWhere(0, "AREA BETWEEN 20 AND 40 AND TYPE <> 'maison'");
    CHECK(q.matched == 2);   // 20 immeuble, 40 annexe; 30 is a maison

    // The cap: 2, three match: truncated, and `matched` says 3.
    q = map.queryWhere(0, "AREA > 25", 2);
    CHECK(q.truncated && q.matched == 3 && q.hits.size() == 2);

    // Said, not crashed: an unreadable expression, a field that is not there, no layer.
    CHECK(!map.queryWhere(0, "AREA >").error.empty());
    CHECK(contains(map.queryWhere(0, "SURFACE > 3").error, "SURFACE"));
    CHECK(!map.queryWhere(3, "AREA > 1").error.empty());

    // Rendering after it: the spatial filter is set again, nothing broken.
    map.setVisible(0, true);
    std::string error;
    auto img = CairoImage::create(ImageSize{100, 100}, error);
    img->canvas().clear(Color::fromHex(0xFFFFFF));
    const View v = *makeView(Envelope{149995, 159995, 150015, 160015}, ImageSize{100, 100});
    const MapReport r = map.render(img->canvas(), v);
    CHECK(!r.layers.empty() && r.layers[0].stats.read.drawn == 1);   // only the first square, in view
}

} // namespace

int main() {
    const Files f = writeFiles();
    testOpen(f);
    testEmpty(f);
    testRender(f);
    testHidden(f);
    testPixelRatioRoundTrip(f);
    testStop(f);
    testReportLines(f);
    testQueryAt(f);
    testQueryArea(f);
    testAddRemoveMove(f);
    testColourFollows(f);
    testQueryWhere();
    return check::summary();
}
