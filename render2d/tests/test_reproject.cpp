// SPDX-License-Identifier: AGPL-3.0-or-later
// Reprojection on the fly (notions §69, §70), with the numbers of the notes:
// the Grand-Place in three systems, the axis order, the filter's rectangle,
// and a map whose layers are in different systems.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "backend/cairo/cairo_image.hpp"
#include "backend/gdal/gdal_reproject.hpp"
#include "check.hpp"
#include "map/config.hpp"
#include "map/map.hpp"

using namespace r2d;

namespace {

std::string write(const std::string& name, const std::string& text) {
    const auto path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream(path) << text;
    return path;
}

CrsInfo crs(const char* text) {
    std::string error;
    auto c = crsFromUser(text, error);
    return c ? *c : CrsInfo{};
}

// EPSG:4326 as a layer gives it: crsFromUser refuses degrees, so it is
// read from a GeoJSON (GDAL: EPSG:4326 when there is no "crs").
CrsInfo wgs84() {
    const std::string path = write("r2d_rp_wgs84.geojson",
        R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},)"
        R"("geometry":{"type":"Point","coordinates":[4.35236,50.846777]}}]})");
    std::string error;
    auto layer = GdalLayer::open(path, "", error);
    return layer ? layer->info().crs : CrsInfo{};
}

// The centre of the Grand-Place (§69), longitude first: x, as in the files.
Geometry grandPlace() {
    Geometry g;
    g.type = GeomType::Point;
    g.points.push_back({4.352360, 50.846777});
    g.closePart(0);
    return g;
}

void testCrsFromUser() {
    std::string error;
    CHECK(crsFromUser("EPSG:31370", error).has_value());
    CHECK(crs("EPSG:31370").epsg == 31370 && !crs("EPSG:31370").geographic);
    CHECK(!crsFromUser("EPSG:4326", error));                         // degrees: refused (§70)
    CHECK(error.find("degrees") != std::string::npos);
    CHECK(!crsFromUser("EPSG:999999", error));                       // unknown
    CHECK(webMercator().epsg == 3857);
}

void testGrandPlace() {
    const CrsInfo w = wgs84();
    CHECK(w.known && w.geographic);
    std::string error;

    // Web Mercator: pure arithmetic, to the millimetre (§69).
    auto toMerc = CrsTransform::create(w, crs("EPSG:3857"), std::nullopt, error);
    CHECK(toMerc != nullptr);
    if (toMerc) {
        Geometry g = grandPlace();
        CHECK(toMerc->forward(g));
        CHECK_NEAR(g.points[0].x, 484502.499, 0.01);
        CHECK_NEAR(g.points[0].y, 6594234.971, 0.01);
    }

    // Lambert 72: the datum change, to the metre (the way PROJ takes may
    // change with its version and the grids installed).
    auto toLambert = CrsTransform::create(w, crs("EPSG:31370"), Envelope{4.3, 50.8, 4.4, 50.9}, error);
    CHECK(toLambert != nullptr);
    if (toLambert) {
        Geometry g = grandPlace();
        CHECK(toLambert->forward(g));
        CHECK_NEAR(g.points[0].x, 148845.6, 1.0);
        CHECK_NEAR(g.points[0].y, 170708.2, 1.0);
        // Not the naive answer, 107.8 m away (§69).
        CHECK(std::abs(g.points[0].x - 148934.7) > 50.0);
        CHECK(toLambert->description().find("EPSG:4326 -> EPSG:31370") == 0);
        CHECK(toLambert->description().find("BD72") != std::string::npos);
        std::printf("     %s\n", toLambert->description().c_str());

        // The filter's rectangle (§70): the view of the Grand-Place, in degrees.
        const auto r = toLambert->backwardBounds({148600, 170500, 149100, 170900});
        CHECK(r.has_value());
        if (r) {
            CHECK_NEAR(r->xmin, 4.3489, 0.0002);
            CHECK_NEAR(r->xmax, 4.3560, 0.0002);
            CHECK_NEAR(r->ymin, 50.8449, 0.0002);
            CHECK_NEAR(r->ymax, 50.8485, 0.0002);
        }
    }
}

void testSame() {
    CHECK(sameCrs(crs("EPSG:31370"), crs("EPSG:31370")));
    CHECK(!sameCrs(crs("EPSG:31370"), crs("EPSG:3857")));
    CHECK(sameCrs(CrsInfo{}, crs("EPSG:3857")));   // not declared: assumed the map's
}

// The same square of buildings, once in Lambert, once in degrees.
const char* kLambert = R"({"type":"FeatureCollection",
 "crs":{"type":"name","properties":{"name":"EPSG:31370"}},
 "features":[{"type":"Feature","properties":{"id":1},"geometry":{"type":"Polygon","coordinates":[
   [[148800,170680],[148890,170680],[148890,170740],[148800,170740],[148800,170680]]]}}]})";

void testMap() {
    const std::string lambert = write("r2d_rp_lambert.geojson", kLambert);

    // The same square, its corners converted to degrees by the transform itself.
    std::string error;
    auto back = CrsTransform::create(crs("EPSG:31370"), wgs84(), std::nullopt, error);
    CHECK(back != nullptr);
    if (!back) return;
    Geometry sq;
    sq.type   = GeomType::Polygon;
    sq.points = {{148800, 170680}, {148890, 170680}, {148890, 170740}, {148800, 170740}, {148800, 170680}};
    sq.closePart(0);
    CHECK(back->forward(sq));
    std::string coords;
    char buf[64];
    for (const Point& p : sq.points) {
        std::snprintf(buf, sizeof buf, "%s[%.10f,%.10f]", coords.empty() ? "" : ",", p.x, p.y);
        coords += buf;
    }
    const std::string degrees = write("r2d_rp_degrees.geojson",
        R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"id":2},)"
        R"("geometry":{"type":"Polygon","coordinates":[[)" + coords + "]]}}]}");

    // A Lambert map: the degrees layer is converted; its extent falls on the square.
    Map map = Map::open({{lambert}, {degrees}});
    CHECK(map.crs().epsg == 31370);
    CHECK(map.layerReprojection(0).empty());
    CHECK(map.layerReprojection(1).find("EPSG:4326 -> EPSG:31370") == 0);
    const auto e = map.layerExtent(1);
    CHECK(e.has_value());
    if (e) {   // the rectangle around the degrees' rectangle: a few cm larger (a turned shape)
        CHECK_NEAR(e->xmin, 148800, 0.05);
        CHECK_NEAR(e->ymax, 170740, 0.05);
    }

    // Drawn, and found by a click in the middle, in both layers.
    auto img = CairoImage::create(ImageSize{100, 100}, error);
    const View v = *makeView(Envelope{148780, 170660, 148910, 170760}, ImageSize{130, 100});
    const MapReport r = map.render(img->canvas(), v);
    CHECK(r.layers.size() == 2 && r.layers[1].stats.read.drawn == 1);
    CHECK(r.layers.size() == 2 && r.layers[1].reprojection.find("EPSG:4326") == 0);
    const auto hits = map.queryAt(v, v.worldToImage.apply({148845, 170710}));
    CHECK(hits.size() == 2);
    if (hits.size() == 2) CHECK_NEAR(hits[0].geometry.points[0].x, 148800, 0.01);   // in map units

    // A map in Mercator: both converted; Mercator metres are 1.584 ground metres (§69).
    Map merc = Map::open({{lambert}, {degrees}}, 1.0, crs("EPSG:3857"));
    const auto em = merc.layerExtent(0);
    CHECK(em.has_value());
    if (em) CHECK_NEAR(em->width() / 90.0, 1.584, 0.01);

    // The first layer in degrees and no --crs: EPSG:3857, said.
    Map auto3857 = Map::open({{degrees}, {lambert}});
    CHECK(auto3857.crs().epsg == 3857);
    bool said = false;
    for (const auto& w : auto3857.warnings()) said = said || w.find("EPSG:3857") != std::string::npos;
    CHECK(said);
}

// GDAL says EPSG:4326 for any GeoJSON without "crs": a hand-made one in
// Lambert metres is not believed (§70).
void testNotReallyDegrees() {
    const std::string lambert = write("r2d_rp_lambert.geojson", kLambert);
    const std::string noCrs = write("r2d_rp_nocrs.geojson",
        R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},)"
        R"("geometry":{"type":"Point","coordinates":[148845,170708]}}]})");
    Map map = Map::open({{lambert}, {noCrs}});
    CHECK(map.crs().epsg == 31370);
    CHECK(map.layerReprojection(1).empty());
    bool said = false;
    for (const auto& w : map.warnings()) said = said || w.find("but its coordinates are not") != std::string::npos;
    CHECK(said);
}

void testConfig() {
    const std::string lambert = write("r2d_rp_lambert.geojson", kLambert);
    const std::string path = write("r2d_rp_map.yaml", "crs: EPSG:3857\nlayers:\n  - source: " + lambert + "\n");
    const ConfigResult r = loadConfig(path);
    CHECK(r.ok && r.config.crs == "EPSG:3857");
    CHECK(configText(r.config, path).find("crs: 'EPSG:3857'") != std::string::npos);
    const ConfigResult bad = loadConfig(write("r2d_rp_bad.yaml", "crs: EPSG:4326\nlayers:\n  - source: x.fgb\n"));
    CHECK(!bad.ok && bad.error.find("degrees") != std::string::npos);
}

} // namespace

int main() {
    testCrsFromUser();
    testGrandPlace();
    testSame();
    testMap();
    testNotReallyDegrees();
    testConfig();
    return check::summary();
}
