// SPDX-License-Identifier: AGPL-3.0-or-later
// The printed page (notions §74): the worked examples of the notes. An A4 at
// 1:1 000 whose frame is 277 mm and shows 277 m; the pixel ratio of paper
// that gives exactly that scale; a 50 m bar of 50 mm; the true north in
// Lambert 72, in EPSG:3034 and in Web Mercator; the area fitted at a nice
// scale; the page drawn, the map clipped to its frame; print: in a map file.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "backend/cairo/cairo_image.hpp"
#include "backend/gdal/gdal_reproject.hpp"
#include "check.hpp"
#include "map/config.hpp"
#include "map/map.hpp"
#include "map/page.hpp"

using namespace r2d;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::string writeText(const char* name, const std::string& text) {
    const std::string path = (fs::temp_directory_path() / name).string();
    std::ofstream(path) << text;
    return path;
}

// A 1 km square at the Grand-Place's south-west, in Lambert 72.
std::string square() {
    return writeText("r2d_page_square.geojson", R"({"type":"FeatureCollection",
 "crs":{"type":"name","properties":{"name":"urn:ogc:def:crs:EPSG::31370"}},
 "features":[{"type":"Feature","properties":{},"geometry":{"type":"Polygon",
   "coordinates":[[[148000,170000],[149000,170000],[149000,171000],[148000,171000],[148000,170000]]]}}]})");
}

void testFormats() {
    PageSpec p;
    std::string error;
    CHECK(parsePageFormat("A4-landscape", p, error) && p.format == "A4" && p.landscape);
    CHECK(parsePageFormat("a3-Portrait", p, error) && p.format == "A3" && !p.landscape);
    CHECK(parsePageFormat("A4", p, error) && !p.landscape);   // as paper is
    CHECK(!parsePageFormat("A5-landscape", p, error) && contains(error, "A4 or A3"));
    CHECK(!parsePageFormat("A4-sideways", p, error) && contains(error, "portrait or landscape"));
    CHECK(scaleText(1000) == "1:1 000" && scaleText(25000) == "1:25 000" && scaleText(500) == "1:500");
    CHECK(std::abs(kPointMm - 0.352778) < 1e-6 && std::abs(kPixelPerPoint - 0.793701) < 1e-6);
}

// §74's worked example: A4 landscape, 1:1 000.
void testPlan() {
    Map map = Map::open({LayerSpec(square())});
    PageSpec spec;
    spec.scale  = 1000;
    spec.center = Point{148500, 170500};
    std::string error;
    const auto plan = planPage(map, spec, std::nullopt, error);
    CHECK(plan.has_value());
    if (!plan) return;
    CHECK_NEAR(plan->width, 841.89, 0.01);    // 297 mm
    CHECK_NEAR(plan->height, 595.28, 0.01);   // 210 mm
    CHECK(plan->frameW == 785 && plan->frameH == 425);   // 277 x 150 mm, in whole points
    CHECK_NEAR(plan->view.resolution, 0.35278, 0.0001);   // a point of paper, 0.3528 m of ground (x 1.00004)
    CHECK_NEAR(plan->view.scaleDenominator(), 1000.0, 0.1);   // with the ratio 0.7937: exactly
    CHECK_NEAR(plan->view.shown.width(), 785 * plan->view.resolution, 1e-6);   // 277 m of ground
    CHECK(plan->warnings.empty());
    const ScaleBar bar = scaleBar(*plan);
    CHECK(bar.ground == 50.0);                 // a fifth of 277 m: 55.4, rounded down to 50
    CHECK_NEAR(bar.points, 141.73, 0.01);      // 50 mm
    CHECK(plan->north && std::abs(*plan->north) < 0.1);   // Lambert 72 at Brussels: straight up
    std::printf("     A4 1:1000: frame %.0f x %.0f pt, %.4f m/pt, bar %g m = %.2f pt, north %+.3f deg\n",
                plan->frameW, plan->frameH, plan->view.resolution, bar.ground, bar.points, *plan->north);

    // Without a scale: the area fitted, the scale rounded up to a nice one.
    PageSpec fit;
    const auto f = planPage(map, fit, Envelope{148000, 170000, 149000, 170500}, error);
    CHECK(f && f->scale == 5000);   // max(1000 / 0.277, 500 / 0.150) = 3610, next nice: 5000
    PageSpec portrait;
    parsePageFormat("A3-portrait", portrait, error);
    const auto a3 = planPage(map, portrait, std::nullopt, error);   // the map's extent, the 1 km square
    CHECK(a3 && a3->scale == 5000 && a3->frameW == 785);            // 1000 / 0.277 = 3610 -> 5000; 277 mm wide
}

// The true north and the ground: EPSG:3034 turns it, Mercator stretches the scale.
void testSystems() {
    std::string error;
    PageSpec spec;
    spec.scale  = 5000;
    spec.center = Point{148500, 170500};
    {   // EPSG:3034: the centre in its own coordinates
        Map lcc = Map::open({LayerSpec(square())}, 1.0, crsFromText("EPSG:3034", error));
        spec.center.reset();
        const auto plan = planPage(lcc, spec, std::nullopt, error);
        CHECK(plan && plan->north && std::abs(*plan->north - 4.38) < 0.05);   // clockwise: towards the east
        CHECK(plan && std::abs(1.0 / plan->mapPerGround - 1.036) < 0.002);    // 1 map metre = 1.036 m of ground
        CHECK(plan && plan->warnings.empty());   // a district: the scale does not change across it
        if (plan) std::printf("     EPSG:3034: north %+.2f deg, 1 map m = %.4f m of ground\n", *plan->north,
                              1.0 / plan->mapPerGround);
    }
    {   // Web Mercator: 1:5 000 on the ground is 1:7 903 in Mercator metres
        Map merc = Map::open({LayerSpec(square())}, 1.0, crsFromText("EPSG:3857", error));
        const auto plan = planPage(merc, spec, std::nullopt, error);
        CHECK(plan && std::abs(plan->mapPerGround - 1.0 / std::cos(50.85 * 3.14159265358979 / 180.0)) < 0.01);
        CHECK(plan && std::abs(plan->view.scaleDenominator() - 5000 * plan->mapPerGround) < 1.0);
        CHECK(plan && plan->warnings.empty());   // 750 m north to south: Mercator's scale holds
        PageSpec large;
        parsePageFormat("A3-landscape", large, error);
        large.scale = 1e6;
        const auto belgium = planPage(merc, large, std::nullopt, error);
        CHECK(belgium && belgium->warnings.size() == 1 && contains(belgium->warnings[0], "changes by 4."));
    }
}

// The page drawn: white sheet, the map inside its frame only, the frame's border.
void testCompose() {
    Map map = Map::open({LayerSpec(square())});
    PageSpec spec;
    spec.scale  = 5000;
    spec.center = Point{148500, 170500};
    spec.title  = "Test";
    spec.date   = "2026-11-03";
    std::string error;
    const auto plan = planPage(map, spec, std::nullopt, error);
    CHECK(plan.has_value());
    if (!plan) return;
    auto img = CairoImage::create(ImageSize{842, 595}, error);   // a point per pixel
    const MapReport rep = composePage(map, spec, *plan, img->canvas());
    CHECK(map.pixelRatio() == 1.0);   // put back
    auto px = [&](int x, int y) { return img->pixels()[y * img->stride() + x * 4 + 1]; };   // green
    CHECK(px(5, 5) == 255);                                    // the margin: white paper
    const Point inside = plan->view.worldToImage.apply({148500, 170500});   // the square's middle
    CHECK(px(static_cast<int>(plan->frameX + inside.x), static_cast<int>(plan->frameY + inside.y)) < 250);   // filled
    CHECK(px(static_cast<int>(plan->frameX) - 6, static_cast<int>(plan->frameY) + 50) == 255);   // left of the frame: nothing
    CHECK(px(static_cast<int>(plan->frameX), static_cast<int>(plan->frameY) + 50) < 120);       // the border
    CHECK(rep.layers.size() == 1);
}

// print: in a map file, read and written back.
void testConfig() {
    const std::string yaml = writeText("r2d_page_map.yaml", "layers:\n"
                                       "  - source: parcels.shp\n"
                                       "print:\n"
                                       "  page: A3-portrait\n"
                                       "  scale: 2500\n"
                                       "  center: [148900, 170700]\n"
                                       "  title: Grand-Place\n"
                                       "  subtitle: UrbIS parcels\n"
                                       "  legend: false\n");
    const ConfigResult r = loadConfig(yaml);
    CHECK(r.ok && r.config.print.has_value());
    if (!r.config.print) return;
    const PageSpec& p = *r.config.print;
    CHECK(p.format == "A3" && !p.landscape && p.scale == 2500.0 && p.center && p.center->x == 148900.0);
    CHECK(p.title == "Grand-Place" && p.subtitle == "UrbIS parcels" && !p.legend);
    const std::string text = configText(r.config, yaml);
    CHECK(contains(text, "print:\n  page: A3-portrait\n  scale: 2500\n  center: [148900, 170700]"));
    CHECK(contains(text, "title: 'Grand-Place'"));
    const ConfigResult bad = loadConfig(writeText("r2d_page_bad.yaml", "layers:\n  - source: a.shp\nprint:\n  page: A5\n"));
    CHECK(!bad.ok && contains(bad.error, "A4 or A3"));
}

// §74 (5b): the legend. Written as files, drawn without the network.
const fs::path kSource = fs::path(R2D_SOURCE_DIR);

std::string lines(const char* name, const std::vector<std::pair<std::string, std::string>>& roads) {
    std::string f = R"({"type":"FeatureCollection","crs":{"type":"name","properties":{"name":"urn:ogc:def:crs:EPSG::31370"}},"features":[)";
    for (std::size_t k = 0; k < roads.size(); ++k)
        f += std::string(k ? "," : "") + R"({"type":"Feature","properties":{"fclass":")" + roads[k].first +
             R"("},"geometry":{"type":"LineString","coordinates":)" + roads[k].second + "}}";
    return writeText(name, f + "]}");
}

void testLegend() {
    const std::string roads = lines("r2d_page_roads.geojson", {{"primary", "[[148100,170500],[148900,170500]]"},
                                                               {"residential", "[[148500,170100],[148500,170900]]"},
                                                               {"footway", "[[148200,170200],[148800,170800]]"}});
    const std::string classes = writeText("r2d_page_classes.yaml", R"(name: communes
feature-styles:
- rules:
  - name: Evere
    filter: ${commune = 'Evere'}
    symbolizers:
    - polygon: {fill-color: '#C98B6B'}
  - name: Schaerbeek
    filter: ${commune = 'Schaerbeek'}
    symbolizers:
    - polygon: {fill-color: '#8BB07A'}
  - name: (other)
    else: true
    symbolizers:
    - polygon: {fill-color: '#DDDDDD'}
)");
    const std::string labelsOnly = writeText("r2d_page_text.yaml", R"(name: labels
symbolizers:
- text: {label: '${id}'}
)");
    LayerSpec parcels(square(), classes);
    parcels.title = "parcels";
    LayerSpec road(roads, (kSource / "styles" / "osm" / "osm-roads.yaml").string());
    road.title = "roads";
    LayerSpec helper(square(), labelsOnly);   // text only: nothing to explain
    LayerSpec hidden(square());
    hidden.legend = false;
    LayerSpec osm((kSource / "basemaps" / "osm.xml").string());   // opened, never drawn here
    std::string error;
    Map map = Map::open({osm, parcels, road, helper, hidden}, 1.0, crsFromText("EPSG:31370", error));   // OSM first: say the system
    CHECK(!map.layerInLegend(0) && map.layerInLegend(1) && !map.layerInLegend(4));   // the basemap, legend: false

    PageSpec spec;
    spec.scale  = 5000;
    spec.center = Point{148500, 170500};
    const auto plan = planPage(map, spec, std::nullopt, error);
    CHECK(plan.has_value());
    if (!plan) return;

    // From the styles alone (no report): every rule active at 1:5 000.
    const auto all = legendEntries(map, plan->view, kPixelPerPoint);
    CHECK(all.size() == 2);   // top first: roads, then parcels; not the helper, the hidden one nor OSM
    if (all.size() == 2) {
        CHECK(all[0].title == "roads" && all[0].lines.size() == 13);
        CHECK(all[1].title == "parcels" && all[1].lines.size() == 3 && all[1].lines[2].label == "(other)");
        const LegendLine* primary = nullptr;
        for (const auto& l : all[0].lines) if (l.label == "primary") primary = &l;
        CHECK(primary && primary->symbols.size() == 2 && primary->sample == GeomType::Line);   // casing, then fill
    }
    std::printf("     legend (styles): %s\n", legendText(all).c_str());

    // At 1:30 000 the footways (up to 1:5 000) are not explained.
    PageSpec far = spec;
    far.scale = 30000;
    const auto farPlan = planPage(map, far, std::nullopt, error);
    bool footway = false;
    for (const auto& g : legendEntries(map, farPlan->view, kPixelPerPoint))
        for (const auto& l : g.lines) if (l.label == "footway") footway = true;
    CHECK(!footway);

    // From the page drawn: only what is on it (OSM hidden: no network in a test).
    map.setVisible(0, false);
    auto img = CairoImage::create(ImageSize{842, 595}, error);
    const MapReport rep = composePage(map, spec, *plan, img->canvas());
    const auto seen = legendEntries(map, plan->view, kPixelPerPoint, &rep);
    CHECK(seen.size() == 2);
    if (seen.size() == 2) {
        CHECK(seen[0].lines.size() == 3);   // the three roads of the file, of 13 kinds
        CHECK(seen[1].lines.size() == 1 && seen[1].lines[0].label == "parcels");   // no commune: only (other) drew: the layer itself
    }
    std::printf("     legend (page): %s\n", legendText(seen).c_str());
    for (const auto& w : rep.warnings) CHECK(!contains(w, "legend"));   // the square has no 'commune': said, but not about the legend
}

// A style without filters is one symbol: its rules stacked on one line.
void testLegendStacked() {
    Map map = Map::open({LayerSpec(square(), (kSource / "styles" / "parcelles.yaml").string())});
    PageSpec spec;
    spec.scale  = 1000;
    spec.center = Point{148500, 170500};
    std::string error;
    const auto plan = planPage(map, spec, std::nullopt, error);
    const auto groups = legendEntries(map, plan->view, kPixelPerPoint);
    CHECK(groups.size() == 1 && groups[0].lines.size() == 1);   // "fond" and "limites": one parcel
    if (groups.size() == 1 && groups[0].lines.size() == 1) {
        CHECK(groups[0].lines[0].label == "r2d_page_square");      // the layer's name
        CHECK(groups[0].lines[0].symbols.size() == 2);             // the ground, then the boundary
    }
}

// 20 classes and their title on an A4 landscape: 20 places, 1 line left out, said.
void testLegendRoom() {
    std::string many = "name: many\nfeature-styles:\n- rules:\n";
    for (int k = 1; k <= 20; ++k)
        many += "  - name: class " + std::to_string(k) + "\n    filter: ${" + std::to_string(k) + " > 0}\n"
                "    symbolizers:\n    - polygon: {fill-color: '#88AACC'}\n";   // filtered: classes, not one symbol
    LayerSpec layer(square(), writeText("r2d_page_many.yaml", many));
    Map map = Map::open({layer});
    PageSpec spec;
    spec.scale  = 5000;
    spec.center = Point{148500, 170500};
    std::string error;
    const auto plan = planPage(map, spec, std::nullopt, error);
    auto img = CairoImage::create(ImageSize{842, 595}, error);
    const MapReport rep = composePage(map, spec, *plan, img->canvas());
    CHECK(rep.warnings.size() == 1 && contains(rep.warnings[0], "legend: 1 line not shown"));
    spec.legend = false;
    CHECK(composePage(map, spec, *plan, img->canvas()).warnings.empty());
}

} // namespace

int main() {
    testFormats();
    testPlan();
    testSystems();
    testCompose();
    testConfig();
    testLegend();
    testLegendRoom();
    testLegendStacked();
    return check::summary();
}
