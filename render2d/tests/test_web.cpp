// SPDX-License-Identifier: AGPL-3.0-or-later
// Web basemaps (notions §73), without the network: what render2d does
// around GDAL's WMS and WMTS drivers. The <Cache /> added, the readable
// names, the identity, OpenStreetMap clipped to Belgium on a Lambert map,
// the extent without the basemaps. Opening a description downloads nothing;
// drawing would (tested by hand, §73).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "backend/cairo/cairo_image.hpp"
#include "backend/gdal/gdal_layer.hpp"
#include "backend/gdal/gdal_raster.hpp"
#include "backend/gdal/gdal_reproject.hpp"
#include "check.hpp"
#include "map/config.hpp"
#include "map/map.hpp"

using namespace r2d;
namespace fs = std::filesystem;

namespace {

const fs::path kBasemaps = fs::path(R2D_SOURCE_DIR) / "basemaps";

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::string writeText(const char* name, const std::string& text) {
    const std::string path = (fs::temp_directory_path() / name).string();
    std::ofstream(path) << text;
    return path;
}

void testCache() {
    const std::string wms = "<GDAL_WMS>\n  <Service name=\"WMS\"/>\n</GDAL_WMS>\n";
    const std::string with = withWebCache(wms);
    CHECK(contains(with, "<Cache />"));
    CHECK(with.find("<Cache />") < with.find("</GDAL_WMS>"));
    CHECK(withWebCache(with) == with);   // once only
    CHECK(withWebCache("<GDAL_WMS><Cache><Path>x</Path></Cache></GDAL_WMS>").find("<Cache />") == std::string::npos);
    CHECK(contains(withWebCache("<GDAL_WMTS><Layer>a</Layer></GDAL_WMTS>"), "<Cache />"));
    CHECK(withWebCache("not a description") == "not a description");
}

void testNames() {
    const std::string wmts = "WMTS:https://geoservices-grid.irisnet.be/geowebcache/service/wmts?service=wmts&request="
                             "GetCapabilities&version=1.0.0,layer=Ortho2024NS,tilematrixset=EPSG:31370";
    CHECK(webLayerName(wmts) == "Ortho2024NS");
    const std::string wms = "WMS:https://geoservices-urbis.irisnet.be/geoserver/BaseMaps/wms?SERVICE=WMS&VERSION=1.1.1"
                            "&REQUEST=GetMap&LAYERS=UrbISFrenchLabeledColor&SRS=EPSG:31370&BBOX=1,2,3,4";
    CHECK(webLayerName(wms) == "UrbISFrenchLabeledColor");
    CHECK(webLayerName("WMS:https://x/wms?LAYERS=a,b&SRS=EPSG:31370") == "a");
    CHECK(webLayerName("parcels.shp").empty());
    CHECK(webLayerName("WMTS:https://x/wmts?request=GetCapabilities").empty());
}

void testIdentity() {
    setWebIdentity("render2d-test/1.0 (+https://github.com/nfr25/render2d)");
    bool said = false;
    for (const auto& line : gdalDataInfo())
        if (contains(line, "web: render2d-test/1.0")) said = true;
    CHECK(said);
    const std::string cache = webCacheFolder();
    CHECK(!cache.empty() && fs::path(cache).filename() == "web");
    std::printf("     web cache: %s\n", cache.c_str());
}

void testOpen() {
    std::string error;
    auto osm = GdalRaster::open((kBasemaps / "osm.xml").string(), error);
    CHECK(osm != nullptr);
    if (!osm) { std::printf("     %s\n", error.c_str()); return; }
    CHECK(osm->info().web);
    CHECK(osm->info().name == "osm");
    CHECK(osm->hasOverviews());
    CHECK(osm->info().crs.epsg == 3857);

    // An XML description without <Cache />: opened all the same, web.
    const std::string bare = writeText("r2d_web_bare.xml", R"(<GDAL_WMS>
  <Service name="WMS">
    <Version>1.1.1</Version>
    <ServerUrl>https://example.invalid/wms?</ServerUrl>
    <Layers>plan</Layers>
    <SRS>EPSG:31370</SRS>
    <ImageFormat>image/png</ImageFormat>
  </Service>
  <DataWindow>
    <UpperLeftX>140000</UpperLeftX><UpperLeftY>180000</UpperLeftY>
    <LowerRightX>160000</LowerRightX><LowerRightY>160000</LowerRightY>
    <SizeX>400000</SizeX><SizeY>400000</SizeY>
  </DataWindow>
  <BandsCount>3</BandsCount>
</GDAL_WMS>
)");
    auto plain = GdalRaster::open(bare, error);
    CHECK(plain && plain->info().web && plain->info().name == "r2d_web_bare");

    // A WMS address: its description is rebuilt in memory, with a cache.
    auto urbis = GdalRaster::open("WMS:https://geoservices-urbis.irisnet.be/geoserver/BaseMaps/wms?SERVICE=WMS"
                                  "&VERSION=1.1.1&REQUEST=GetMap&LAYERS=UrbISFrenchLabeledColor&SRS=EPSG:31370"
                                  "&BBOX=21991.63253589799,21162.16053197249,295167.070044775,244027.2116964629",
                                  error);
    CHECK(urbis && urbis->info().web && urbis->info().name == "UrbISFrenchLabeledColor");
    if (!urbis) std::printf("     %s\n", error.c_str());

    // The ready UrbIS files: 5 cm at the finest, 12 levels.
    for (const char* f : {"urbis-fr.xml", "urbis-plain-grey.xml"}) {
        auto r = GdalRaster::open((kBasemaps / f).string(), error);
        CHECK(r && r->info().web && r->info().crs.epsg == 31370);
        if (r) CHECK_NEAR((r->info().extent->xmax - r->info().extent->xmin) / r->width(), 0.05, 0.0001);
    }
}

// §73: OpenStreetMap (the world) on a Lambert map: warped over Belgium only,
// and the map's extent is the data's.
void testClip() {
    const std::string square = writeText("r2d_web_square.geojson", R"({"type":"FeatureCollection",
 "crs":{"type":"name","properties":{"name":"urn:ogc:def:crs:EPSG::31370"}},
 "features":[{"type":"Feature","properties":{},"geometry":{"type":"Polygon",
   "coordinates":[[[148000,170000],[149000,170000],[149000,171000],[148000,171000],[148000,170000]]]}}]})");
    Map map = Map::open({{square, {}}, {(kBasemaps / "osm.xml").string(), {}}});
    CHECK(map.layerCount() == 2);
    CHECK(map.crs().epsg == 31370);
    CHECK(contains(map.layerReprojection(1), "EPSG:3857 -> EPSG:31370"));
    const auto e = map.layerExtent(1);
    CHECK(e.has_value());
    if (!e) return;
    std::printf("     OSM in Lambert, clipped: %.0f %.0f %.0f %.0f (%.0f x %.0f km)\n", e->xmin, e->ymin, e->xmax, e->ymax,
                (e->xmax - e->xmin) / 1000, (e->ymax - e->ymin) / 1000);
    CHECK(e->xmax - e->xmin > 250e3 && e->xmax - e->xmin < 400e3);   // Belgium, not 260,000 km
    CHECK(e->ymax - e->ymin > 200e3 && e->ymax - e->ymin < 300e3);
    CHECK(e->xmin < 149000 && e->xmax > 149000 && e->ymin < 170000 && e->ymax > 170000);   // Brussels inside
    const auto all = map.extent();
    CHECK(all.has_value());
    if (all) {   // the square, not Belgium
        CHECK_NEAR(all->xmin, 148000, 0.01);
        CHECK_NEAR(all->xmax, 149000, 0.01);
    }

    // Alone, on a Lambert map: the extent is the clipped one.
    std::string error;
    Map alone = Map::open({{(kBasemaps / "osm.xml").string(), {}}}, 1.0, crsFromText("EPSG:31370", error));
    const auto a = alone.extent();
    CHECK(a.has_value() && a->xmax - a->xmin < 400e3);

    // The UrbIS WMS in Lambert on a Lambert map: nothing to warp; Belgium's
    // box, but the extent stays the data's.
    Map urbis = Map::open({{square, {}}, {(kBasemaps / "urbis-plain-grey.xml").string(), {}}});
    CHECK(urbis.layerReprojection(1).empty());
    const auto u = urbis.extent();
    CHECK(u.has_value() && u->xmax - u->xmin < 1001.0);
}

// §73 (4c-2): the attributions. OpenStreetMap's said by its source; one
// from the map file; written in the corner unless switched off.
// NEVER draws a web layer: a test must not depend on the network, nor
// download OpenStreetMap's tiles at every run (their usage policy). It
// did on 2 November, and failed where the network worked: OSM's tiles
// covered the corner.
void testAttribution() {
    auto squareAt = [](const char* name, double x0) {
        return writeText(name, std::string(R"({"type":"FeatureCollection",
 "crs":{"type":"name","properties":{"name":"urn:ogc:def:crs:EPSG::31370"}},
 "features":[{"type":"Feature","properties":{},"geometry":{"type":"Polygon",
   "coordinates":[[[)") + std::to_string(x0) + R"(,170000],[)" + std::to_string(x0 + 1000) + R"(,170000],[)" +
                                     std::to_string(x0 + 1000) + R"(,171000],[)" + std::to_string(x0) +
                                     R"(,171000],[)" + std::to_string(x0) + R"(,170000]]]}}]})");
    };
    LayerSpec parcels(squareAt("r2d_web_square2.geojson", 148000));
    parcels.attribution = "UrbIS © Brussels Region";
    LayerSpec other(squareAt("r2d_web_square3.geojson", 148000));
    other.attribution = "Test © Somebody";

    // OpenStreetMap's: said by its source, opened only (nothing downloaded).
    Map withOsm = Map::open({LayerSpec((kBasemaps / "osm.xml").string()), parcels});
    CHECK(withOsm.layerAttribution(0) == "© OpenStreetMap contributors");
    CHECK(withOsm.layerAttribution(1) == "UrbIS © Brussels Region");

    std::string error;
    Map map = Map::open({parcels, other, parcels});   // the same text twice: written once
    const View view = *makeView({148000, 170000, 149000, 171000}, ImageSize{300, 300});
    auto img = CairoImage::create(ImageSize{300, 300}, error);
    img->canvas().clear(Color::fromHex(0x000000));
    const MapReport rep = map.render(img->canvas(), view);
    CHECK(rep.attributions.size() == 2);
    if (rep.attributions.size() == 2) {   // in drawing order, each once
        CHECK(rep.attributions[0] == "UrbIS © Brussels Region");
        CHECK(rep.attributions[1] == "Test © Somebody");
    }
    bool line = false;
    for (const auto& l : reportLines(rep)) if (contains(l, "attribution: Test © Somebody")) line = true;
    CHECK(line);
    auto at = [](CairoImage& i, int x, int y) { return i.pixels()[y * i.stride() + x * 4 + 1]; };   // green
    const int veil = at(*img, 298, 296);   // the light veil, bottom right
    const int under = at(*img, 150, 150);   // what is drawn elsewhere (the squares' fill)
    CHECK(veil > 150 && veil != under);

    map.setAttributions(false);    // a tile of a pyramid
    img->canvas().clear(Color::fromHex(0x000000));
    const MapReport bare = map.render(img->canvas(), view);
    CHECK(bare.attributions.size() == 2);   // still known (for the web page)...
    CHECK(at(*img, 298, 296) == under);      // ...but not written: the squares' fill in the corner too

    // A layer away from the view: its attribution is not written.
    const View far = *makeView({100000, 100000, 100100, 100100}, ImageSize{50, 50});
    auto small = CairoImage::create(ImageSize{50, 50}, error);
    const MapReport away = Map::open({parcels}).render(small->canvas(), far);
    CHECK(away.attributions.empty());
}

// The map file: attribution: read and written; web sources kept as written
// (not joined to the file's folder like a relative path).
void testConfig() {
    const std::string wmts = "WMTS:https://geoservices-grid.irisnet.be/geowebcache/service/wmts?service=wmts"
                             "&request=GetCapabilities&version=1.0.0,layer=Ortho2024NS,tilematrixset=EPSG:31370";
    const std::string yaml = writeText("r2d_web_map.yaml", "layers:\n"
        "  - source: \"" + wmts + "\"\n"
        "  - source: \"WMS:https://example.invalid/wms?LAYERS=a&SRS=EPSG:31370\"\n"
        "  - source: \"https://example.invalid/data.fgb\"\n"
        "  - source: parcels.shp\n"
        "    attribution: \"UrbIS \u00A9 Brussels Region\"\n");
    const ConfigResult r = loadConfig(yaml);
    CHECK(r.error.empty() && r.config.layers.size() == 4);
    if (r.config.layers.size() != 4) return;
    CHECK(r.config.layers[0].source == wmts);
    CHECK(r.config.layers[1].source.rfind("WMS:https://", 0) == 0);
    CHECK(r.config.layers[2].source == "https://example.invalid/data.fgb");
    CHECK(r.config.layers[3].source != "parcels.shp");   // a file: joined to the folder
    CHECK(r.config.layers[3].attribution == "UrbIS \u00A9 Brussels Region");
    const std::string text = configText(r.config, yaml);
    CHECK(contains(text, "source: '" + wmts + "'"));   // written as read, not made relative
    CHECK(contains(text, "attribution: 'UrbIS \u00A9 Brussels Region'"));
}

} // namespace

int main() {
    testCache();
    testNames();
    testIdentity();
    testOpen();
    testClip();
    testAttribution();
    testConfig();
    return check::summary();
}
