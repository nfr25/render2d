// SPDX-License-Identifier: AGPL-3.0-or-later
// Reading through GDAL: a small GeoJSON layer in Lambert 72, written by the
// test itself, covering every case of notions §15 to §18.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "backend/gdal/gdal_layer.hpp"
#include "check.hpp"

using namespace r2d;

namespace {

// Seven features, in this order:
//   1 Polygon with a courtyard        (2 parts of 5 points)
//   2 MultiPolygon: commune + island  (2 parts)
//   3 MultiLineString: two segments   (2 parts)
//   4 Point
//   5 no geometry                     -> counted as empty
//   6 Polygon far away (200 km)       -> excluded by the spatial filter
//   7 GeometryCollection              -> counted as unsupported
const char* kGeoJson = R"({
 "type": "FeatureCollection",
 "crs": {"type": "name", "properties": {"name": "EPSG:31370"}},
 "features": [
  {"type":"Feature","properties":{"id":1,"kind":"house","area":120.5},"geometry":{"type":"Polygon","coordinates":[
    [[150020,160030],[150020,160170],[150130,160170],[150130,160030],[150020,160030]],
    [[150050,160080],[150050,160130],[150100,160130],[150100,160080],[150050,160080]]]}},
  {"type":"Feature","properties":{"id":2,"kind":null,"area":7},"geometry":{"type":"MultiPolygon","coordinates":[
    [[[150200,160020],[150200,160100],[150280,160100],[150280,160020],[150200,160020]]],
    [[[150300,160050],[150300,160080],[150330,160080],[150330,160050],[150300,160050]]]]}},
  {"type":"Feature","properties":{"id":3},"geometry":{"type":"MultiLineString","coordinates":[
    [[150010,160010],[150080,160015],[150140,160012]],
    [[150160,160015],[150220,160011]]]}},
  {"type":"Feature","properties":{"id":4},"geometry":{"type":"Point","coordinates":[150150,160150]}},
  {"type":"Feature","properties":{"id":5},"geometry":null},
  {"type":"Feature","properties":{"id":6},"geometry":{"type":"Polygon","coordinates":[
    [[200000,200000],[200000,200010],[200010,200010],[200010,200000],[200000,200000]]]}},
  {"type":"Feature","properties":{"id":7},"geometry":{"type":"GeometryCollection","geometries":[
    {"type":"Point","coordinates":[150160,160160]}]}}
 ]
})";

std::string writeTestFile() {
    const auto path = std::filesystem::temp_directory_path() / "r2d_test_layer.geojson";
    std::ofstream(path) << kGeoJson;
    return path.string();
}

void testInfo(const std::string& path) {
    std::string error;
    auto layer = GdalLayer::open(path, "", error);
    CHECK(layer != nullptr);
    if (!layer) { std::printf("  open error: %s\n", error.c_str()); return; }

    const LayerInfo& li = layer->info();
    CHECK(li.crs.known);
    CHECK(li.crs.epsg == 31370);
    CHECK(!li.type.has_value());            // mixed types in one GeoJSON layer
    CHECK(li.extent.has_value());
    if (li.extent) {
        CHECK_NEAR(li.extent->xmin, 150010.0, 1e-6);
        CHECK_NEAR(li.extent->xmax, 200010.0, 1e-6);   // includes the far polygon
    }
}

void testReadAll(const std::string& path) {
    std::string error;
    auto layer = GdalLayer::open(path, "", error);
    if (!layer) { CHECK(false); return; }

    std::vector<Geometry> got;   // copies: the visitor's Geometry is reused
    const ReadStats st = layer->forEach([&](const Geometry& g, const Attributes&, FeatureId) { got.push_back(g); });

    CHECK(st.read == 7);
    CHECK(st.drawn == 5);
    CHECK(st.empty == 1);
    CHECK(st.unsupported == 1);
    if (got.size() != 5) return;

    // 1. Polygon with courtyard: notions §16, exactly the worked example.
    CHECK(got[0].type == GeomType::Polygon);
    CHECK(got[0].points.size() == 10);
    CHECK(got[0].parts.size() == 2);
    CHECK(got[0].parts[0].start == 0 && got[0].parts[0].count == 5);
    CHECK(got[0].parts[1].start == 5 && got[0].parts[1].count == 5);
    CHECK_NEAR(got[0].points[5].x, 150050.0, 0.0);

    // 2. MultiPolygon folded into Polygon, one part per polygon.
    CHECK(got[1].type == GeomType::Polygon);
    CHECK(got[1].parts.size() == 2);

    // 3. MultiLineString folded into Line.
    CHECK(got[2].type == GeomType::Line);
    CHECK(got[2].parts.size() == 2);
    CHECK(got[2].parts[1].start == 3 && got[2].parts[1].count == 2);

    // 4. Point.
    CHECK(got[3].type == GeomType::Point);
    CHECK(got[3].points.size() == 1);

    // 6. The far polygon is read when there is no filter.
    CHECK_NEAR(got[4].points[0].x, 200000.0, 0.0);
}

// notions §18: only features touching the rectangle come back.
void testSpatialFilter(const std::string& path) {
    std::string error;
    auto layer = GdalLayer::open(path, "", error);
    if (!layer) { CHECK(false); return; }

    layer->setSpatialFilter({150000.0, 160000.0, 150400.0, 160400.0});
    bool sawFar = false;
    const ReadStats st = layer->forEach([&](const Geometry& g, const Attributes&, FeatureId) {
        if (g.envelope().xmin >= 200000.0) sawFar = true;
    });
    CHECK(!sawFar);
    CHECK(st.drawn == 4);        // polygon, multipolygon, lines, point
    CHECK(st.unsupported == 1);  // the collection is near: read, not drawn
}

// notions §27, §30: fields and their types; only the asked columns are read.
void testAttributes(const std::string& path) {
    std::string error;
    auto layer = GdalLayer::open(path, "", error);
    if (!layer) { CHECK(false); return; }

    const auto& f = layer->fields();
    int id = -1, kind = -1, area = -1;
    for (int i = 0; i < static_cast<int>(f.size()); ++i) {
        if (f[i].name == "id")   id = i;
        if (f[i].name == "kind") kind = i;
        if (f[i].name == "area") area = i;
    }
    CHECK(id >= 0 && kind >= 0 && area >= 0);
    if (id < 0 || kind < 0 || area < 0) return;
    CHECK(f[id].type == FieldType::Integer);
    CHECK(f[kind].type == FieldType::Text);
    CHECK(f[area].type == FieldType::Real);

    // By default: no attribute at all.
    std::vector<Attributes> rows;
    layer->forEach([&](const Geometry&, const Attributes& a, FeatureId) { rows.push_back(a); });
    CHECK(rows.size() == 5);
    bool allNull = true;
    for (const auto& r : rows) for (const auto& v : r) allNull = allNull && isNull(v);
    CHECK(allNull);

    // Only 'kind' and 'area'.
    layer->setNeededFields({kind, area});
    rows.clear();
    layer->forEach([&](const Geometry&, const Attributes& a, FeatureId) { rows.push_back(a); });
    if (rows.size() < 3) { CHECK(false); return; }
    CHECK(std::get<std::string>(rows[0][kind]) == "house");
    CHECK_NEAR(std::get<double>(rows[0][area]), 120.5, 0.0);
    CHECK(isNull(rows[0][id]));                 // not asked
    CHECK(isNull(rows[1][kind]));               // a real NULL
    CHECK_NEAR(std::get<double>(rows[1][area]), 7.0, 0.0);
    CHECK(isNull(rows[2][kind]));               // feature 3 has no such property
}

// listLayers (editor2d's "Connect to database"): a source with two layers,
// made with GDAL's VRT format (a small XML file pointing at the GeoJSON).
void testListLayers(const std::string& path) {
    const auto vrt = (std::filesystem::temp_directory_path() / "r2d_test_two.vrt").string();
    std::ofstream(vrt) << "<OGRVRTDataSource>"
        "<OGRVRTLayer name=\"everything\"><SrcDataSource>" << path << "</SrcDataSource>"
        "<SrcLayer>r2d_test_layer</SrcLayer></OGRVRTLayer>"
        "<OGRVRTLayer name=\"points\"><SrcDataSource>" << path << "</SrcDataSource>"
        "<SrcSQL>SELECT * FROM r2d_test_layer WHERE id = 4</SrcSQL>"
        "<GeometryType>wkbPoint</GeometryType></OGRVRTLayer>"
        "<OGRVRTLayer name=\"kinds\"><SrcDataSource>" << path << "</SrcDataSource>"
        "<SrcLayer>r2d_test_layer</SrcLayer><GeometryType>wkbNone</GeometryType></OGRVRTLayer>"
        "</OGRVRTDataSource>";
    std::string error;
    const std::vector<LayerInfo> layers = listLayers(vrt, error);
    CHECK(error.empty());
    CHECK(layers.size() == 3);
    if (layers.size() == 3) {
        CHECK(layers[0].name == "everything" && layers[1].name == "points");
        CHECK(!layers[0].type.has_value());                                     // mixed
        CHECK(layers[1].type && *layers[1].type == GeomType::Point);
        CHECK(layers[0].featureCount == 7);
        CHECK(layers[1].featureCount == 1 || layers[1].featureCount == -1);   // an SQL layer: -1 allowed (costly)
        CHECK(layers[0].hasGeometry && layers[1].hasGeometry && !layers[2].hasGeometry);   // a table of attributes
        CHECK(layers[0].crs.epsg == 31370);
        CHECK(layers[0].layersInSource == 3);
    }
    CHECK(listLayers("absent.gpkg", error).empty() && error.find("cannot open") != std::string::npos);
    std::filesystem::remove(vrt);
}

// §67: the session password, added only for GDAL, only to its connection.
void testSessionPassword() {
    const std::string pg = "PG:host=db.example.org dbname=gis user=gisuser connect_timeout=5";
    CHECK(withSessionPassword(pg) == pg);                                  // none yet
    setSessionPassword(pg, "secret");
    CHECK(withSessionPassword(pg) == pg + " password=secret");
    CHECK(withSessionPassword("PG:host=other dbname=gis") == "PG:host=other dbname=gis");   // another server
    CHECK(withSessionPassword("data/batiments.fgb") == "data/batiments.fgb");              // a file
    setSessionPassword(pg, "it's a secret");
    CHECK(withSessionPassword(pg) == pg + " password='it\\'s a secret'");   // libpq's quoting
    setSessionPassword(pg, "");
    CHECK(withSessionPassword(pg) == pg);                                  // forgotten
    // An error message names the source as given, never with the password.
    setSessionPassword("PG:host=nowhere.invalid dbname=x connect_timeout=1", "secret");
    std::string error;
    listLayers("PG:host=nowhere.invalid dbname=x connect_timeout=1", error);
    CHECK(!error.empty() && error.find("secret") == std::string::npos);
}

void testErrors(const std::string& path) {
    std::string error;
    CHECK(GdalLayer::open("does_not_exist.shp", "", error) == nullptr);
    CHECK(error.find("cannot open") != std::string::npos);

    error.clear();
    CHECK(GdalLayer::open(path, "no_such_layer", error) == nullptr);
    CHECK(error.find("not found") != std::string::npos);
}

} // namespace

int main() {
    const std::string path = writeTestFile();
    testInfo(path);
    testReadAll(path);
    testSpatialFilter(path);
    testAttributes(path);
    testErrors(path);
    testListLayers(path);
    testSessionPassword();
    std::filesystem::remove(path);
    return check::summary();
}
