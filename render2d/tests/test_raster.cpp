// SPDX-License-Identifier: AGPL-3.0-or-later
// The rasters (notions §65, §66): a 4 x 4 checkerboard GeoTIFF written by the
// test, read at the size of the view, laid on the image; a grey terrain
// model with a world file and a nodata value, like UrbIS's DSM.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include "backend/cairo/cairo_image.hpp"
#include "backend/gdal/gdal_raster.hpp"
#include "check.hpp"
#include "map/map.hpp"

using namespace r2d;
namespace fs = std::filesystem;

namespace {

// The checkerboard: 4 x 4 cells of 10 m, top-left corner (1000, 2000), Lambert 72.
const Envelope kBoard{1000.0, 1960.0, 1040.0, 2000.0};

// Cell (col, row): R, G, B. Cell (0, 0) is the orthophoto pixel of §66;
// cell (3, 3) is black = nodata on the three bands: transparent.
struct Rgb { int r, g, b; };
Rgb cell(int col, int row) {
    if (col == 0 && row == 0) return {255, 200, 100};
    if (col == 3 && row == 3) return {0, 0, 0};
    return {40 + 50 * col, 40 + 50 * row, 120};
}

fs::path tmp(const char* name) { return fs::temp_directory_path() / name; }

std::string writeBoard(const char* name, const char* epsg = "EPSG:31370") {
    GDALAllRegister();
    const std::string path = tmp(name).string();
    GDALDriver* gtiff = GetGDALDriverManager()->GetDriverByName("GTiff");
    GDALDataset* ds = gtiff->Create(path.c_str(), 4, 4, 3, GDT_Byte, nullptr);
    double gt[6] = {1000.0, 10.0, 0.0, 2000.0, 0.0, -10.0};
    ds->SetGeoTransform(gt);
    OGRSpatialReference srs;
    srs.SetFromUserInput(epsg);
    ds->SetSpatialRef(&srs);
    for (int b = 1; b <= 3; ++b) {
        unsigned char v[16];
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col) {
                const Rgb c = cell(col, row);
                v[row * 4 + col] = static_cast<unsigned char>(b == 1 ? c.r : b == 2 ? c.g : c.b);
            }
        GDALRasterBand* band = ds->GetRasterBand(b);
        CPLErr err = band->RasterIO(GF_Write, 0, 0, 4, 4, v, 4, 4, GDT_Byte, 0, 0);
        CHECK(err == CE_None);
        band->SetNoDataValue(0.0);
    }
    GDALClose(ds);
    return path;
}

// The DSM's corner, 2 x 2 pixels of 0.5 m: no CRS, the position in a world
// file (the centre of the first pixel), nodata -3.4e38 as in UrbIS.
const double kNoData = -3.4028234663852886e38;
std::string writeDsm(bool worldFile) {
    const std::string path = tmp(worldFile ? "r2d_raster_dsm.tif" : "r2d_raster_lost.tif").string();
    GDALDriver* gtiff = GetGDALDriverManager()->GetDriverByName("GTiff");
    GDALDataset* ds = gtiff->Create(path.c_str(), 2, 2, 1, GDT_Float32, nullptr);
    float v[4] = {13.0f, 40.0f, 99.6f, static_cast<float>(kNoData)};
    GDALRasterBand* band = ds->GetRasterBand(1);
    CPLErr err = band->RasterIO(GF_Write, 0, 0, 2, 2, v, 2, 2, GDT_Float32, 0, 0);
    CHECK(err == CE_None);
    band->SetNoDataValue(kNoData);
    GDALClose(ds);
    fs::path tfw = fs::path(path).replace_extension(".tfw");
    if (worldFile) std::ofstream(tfw) << "0.5\n0\n0\n-0.5\n147000.384\n171999.822\n";
    else           fs::remove(tfw);
    fs::remove(path + ".aux.xml");
    return path;
}

std::string writeText(const char* name, const char* text) {
    const std::string path = tmp(name).string();
    std::ofstream(path) << text;
    return path;
}

// Pixel `i` of a block: A, R, G, B.
int alphaOf(std::uint32_t p) { return static_cast<int>(p >> 24); }
int redOf(std::uint32_t p)   { return static_cast<int>((p >> 16) & 0xFF); }
int greenOf(std::uint32_t p) { return static_cast<int>((p >> 8) & 0xFF); }
int blueOf(std::uint32_t p)  { return static_cast<int>(p & 0xFF); }

bool sameColour(std::uint32_t p, Rgb c) {
    return alphaOf(p) == 255 && redOf(p) == c.r && greenOf(p) == c.g && blueOf(p) == c.b;
}

int channel(CairoImage& img, int x, int y, int byte) { return img.pixels()[y * img.stride() + x * 4 + byte]; }

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

bool anyWarning(const std::vector<std::string>& ws, const std::string& part) {
    for (const auto& w : ws) if (contains(w, part)) return true;
    return false;
}

// §65: what the file says.
void testOpen(const std::string& board) {
    std::string error;
    auto r = GdalRaster::open(board, error);
    CHECK(r != nullptr);
    if (!r) { std::printf("     %s\n", error.c_str()); return; }
    const LayerInfo& li = r->info();
    CHECK(li.raster);
    CHECK(!li.type.has_value());
    CHECK(li.rasterKind == "RGB");
    CHECK(li.crs.known && li.crs.epsg == 31370);
    CHECK(li.extent.has_value());
    if (li.extent) {
        CHECK_NEAR(li.extent->xmin, 1000.0, 1e-9);
        CHECK_NEAR(li.extent->ymin, 1960.0, 1e-9);
        CHECK_NEAR(li.extent->xmax, 1040.0, 1e-9);
        CHECK_NEAR(li.extent->ymax, 2000.0, 1e-9);
    }
    CHECK(r->width() == 4 && r->height() == 4);
    CHECK(!r->hasOverviews());
}

// §66, "la preuve": the view covers the board exactly, 4 x 4 pixels.
void testExact(const std::string& board) {
    std::string error;
    auto r = GdalRaster::open(board, error);
    if (!r) return;
    const RasterRead rr = r->read(*makeView(kBoard, ImageSize{4, 4}), error);
    CHECK(error.empty());
    CHECK(rr.x == 0 && rr.y == 0 && rr.block.width == 4 && rr.block.height == 4);
    CHECK(!rr.reduced);
    if (rr.block.argb.size() != 16) return;
    bool all = true;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            if (!(col == 3 && row == 3)) all = all && sameColour(rr.block.argb[row * 4 + col], cell(col, row));
    CHECK(all);
    CHECK(rr.block.argb[15] == 0u);   // nodata: transparent
}

// 8 x 8 pixels of 5 m: each cell is 2 x 2 pixels, no shift.
void testEnlarged(const std::string& board) {
    std::string error;
    auto r = GdalRaster::open(board, error);
    if (!r) return;
    const RasterRead rr = r->read(*makeView(kBoard, ImageSize{8, 8}), error);
    CHECK(rr.block.width == 8 && rr.block.height == 8);
    if (rr.block.argb.size() != 64) return;
    bool all = true;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const std::uint32_t p = rr.block.argb[y * 8 + x];
            all = all && (x / 2 == 3 && y / 2 == 3 ? p == 0u : sameColour(p, cell(x / 2, y / 2)));
        }
    CHECK(all);
}

// Half a cell to the right: x from 1005 to 1045, 8 pixels of 5 m. The border
// between cells 0 and 1 (x = 1010) falls between pixels 0 and 1; the last
// pixel (centre 1042.5) is beyond the board: not drawn.
void testHalfCell(const std::string& board) {
    std::string error;
    auto r = GdalRaster::open(board, error);
    if (!r) return;
    const RasterRead rr = r->read(*makeView({1005.0, 1960.0, 1045.0, 2000.0}, ImageSize{8, 8}), error);
    CHECK(rr.x == 0 && rr.block.width == 7 && rr.block.height == 8);
    if (rr.block.argb.size() != 56) return;
    CHECK(sameColour(rr.block.argb[0], cell(0, 0)));   // centre 1007.5: cell 0
    CHECK(sameColour(rr.block.argb[1], cell(1, 0)));   // centre 1012.5: cell 1
    CHECK(sameColour(rr.block.argb[2], cell(1, 0)));   // centre 1017.5: cell 1
    CHECK(sameColour(rr.block.argb[3], cell(2, 0)));   // centre 1022.5: cell 2
    CHECK(sameColour(rr.block.argb[6], cell(3, 0)));   // centre 1037.5: cell 3
}

// 2 x 2 pixels of 20 m: GDAL averages 2 x 2 cells. The bottom-right pixel
// holds the nodata cell: GDAL averages the 3 others, and the pixel is seen.
void testReduced(const std::string& board) {
    std::string error;
    auto r = GdalRaster::open(board, error);
    if (!r) return;
    const RasterRead rr = r->read(*makeView(kBoard, ImageSize{2, 2}), error);
    CHECK(error.empty());
    CHECK(rr.reduced);
    CHECK_NEAR(rr.fileWidth, 4.0, 1e-9);
    CHECK(rr.block.width == 2 && rr.block.height == 2);
    if (rr.block.argb.size() != 4) return;
    const std::uint32_t tl = rr.block.argb[0];
    const double wantR = (cell(0, 0).r + cell(1, 0).r + cell(0, 1).r + cell(1, 1).r) / 4.0;
    const double wantG = (cell(0, 0).g + cell(1, 0).g + cell(0, 1).g + cell(1, 1).g) / 4.0;
    CHECK(alphaOf(tl) == 255);
    CHECK_NEAR(redOf(tl), wantR, 1.0);
    CHECK_NEAR(greenOf(tl), wantG, 1.0);
    const std::uint32_t br = rr.block.argb[3];
    CHECK(alphaOf(br) == 255);
    CHECK_NEAR(redOf(br), (cell(2, 2).r + cell(3, 2).r + cell(2, 3).r) / 3.0, 1.0);   // 157, nodata left out
    CHECK_NEAR(greenOf(br), (cell(2, 2).g + cell(3, 2).g + cell(2, 3).g) / 3.0, 1.0);
}

// §66: the DSM, grey from the min (13 m) to the max (99.6 m) of the whole
// raster; the world file's half pixel; no CRS.
void testDsm() {
    const std::string dsm = writeDsm(true);
    std::string error;
    auto r = GdalRaster::open(dsm, error);
    CHECK(r != nullptr);
    if (!r) { std::printf("     %s\n", error.c_str()); return; }
    const LayerInfo& li = r->info();
    CHECK(!li.crs.known);
    CHECK(li.rasterKind == "grey 13 to 99.6");
    std::printf("     DSM kind: %s\n", li.rasterKind.c_str());
    if (!li.extent) return;
    CHECK_NEAR(li.extent->xmin, 147000.134, 1e-6);   // 147 000.384 - 0.25
    CHECK_NEAR(li.extent->ymax, 172000.072, 1e-6);   // 171 999.822 + 0.25
    CHECK_NEAR(li.extent->xmax, 147001.134, 1e-6);

    const RasterRead rr = r->read(*makeView(*li.extent, ImageSize{2, 2}), error);
    CHECK(rr.block.argb.size() == 4);
    if (rr.block.argb.size() != 4) return;
    CHECK(sameColour(rr.block.argb[0], {0, 0, 0}));         // 13 m: black
    CHECK(sameColour(rr.block.argb[1], {80, 80, 80}));      // 40 m: 79.5 -> 80
    CHECK(sameColour(rr.block.argb[2], {255, 255, 255}));   // 99.6 m: white
    CHECK(rr.block.argb[3] == 0u);                          // nodata: transparent

    // Without its world file: no position, refused with a clear message.
    const std::string lost = writeDsm(false);
    CHECK(GdalRaster::open(lost, error) == nullptr);
    CHECK(contains(error, "no position on the ground"));
}

// §66: on a map. Opacity 50 % on white gives 255, 227, 177; the lines of a
// vector layer above are drawn over the pixels.
void testOnMap(const std::string& board) {
    const std::string half = writeText("r2d_raster_half.yaml", R"(name: half
feature-styles:
- rules:
  - symbolizers:
    - raster:
        opacity: 0.5
)");
    const std::string near = writeText("r2d_raster_near.yaml", R"(name: near
feature-styles:
- rules:
  - scale: [min, 20000]
    symbolizers:
    - raster: {}
)");
    // A horizontal line at y = 1985, across the board.
    const std::string line = writeText("r2d_raster_line.geojson", R"({
 "type": "FeatureCollection",
 "crs": {"type": "name", "properties": {"name": "EPSG:31370"}},
 "features": [
  {"type":"Feature","properties":{"id":1},"geometry":{"type":"LineString","coordinates":[[995,1985],[1045,1985]]}}
 ]
})");
    const std::string lineStyle = writeText("r2d_raster_line.yaml", R"(name: black
symbolizers:
- line: {stroke-color: '#000000', stroke-width: 2}
)");
    const View view = *makeView(kBoard, ImageSize{40, 40});   // 1 m per pixel
    std::string error;

    {   // opacity
        Map map = Map::open({{board, half}});
        CHECK(map.warnings().empty());
        auto img = CairoImage::create(ImageSize{40, 40}, error);
        img->canvas().clear(Color::fromHex(0xFFFFFF));
        const MapReport rep = map.render(img->canvas(), view);
        CHECK(rep.layers.size() == 1 && rep.layers[0].stats.raster);
        CHECK_NEAR(channel(*img, 5, 5, 2), 255, 1.0);   // R
        CHECK_NEAR(channel(*img, 5, 5, 1), 227, 1.0);   // G
        CHECK_NEAR(channel(*img, 5, 5, 0), 177, 1.0);   // B
        CHECK(channel(*img, 35, 35, 1) == 255);          // nodata: the white below
        const auto lines = reportLines(rep);
        CHECK(lines.size() == 2 && contains(lines[1], "raster 40x40 px from 4x4 file px (nearest)"));
        if (lines.size() > 1) std::printf("     %s\n", lines[1].c_str());
    }
    {   // the line above the raster
        Map map = Map::open({{board, {}}, {line, lineStyle}});
        CHECK(map.warnings().empty());
        auto img = CairoImage::create(ImageSize{40, 40}, error);
        img->canvas().clear(Color::fromHex(0xFFFFFF));
        map.render(img->canvas(), view);
        CHECK(channel(*img, 5, 15, 1) < 30);              // row 15 = y 1984.5: black line
        CHECK(channel(*img, 5, 10, 1) == cell(0, 1).g);   // row 10: the raster
        CHECK(map.queryAt(view, {5.0, 15.0}).size() == 1);   // the line, not the raster
        CHECK(contains(map.queryWhere(0, "id = 1").error, "is a raster"));
        CHECK(map.layerFields(0).empty());
    }
    {   // a rule's scale: 1 m/px is 1:3571 -> drawn; 50 m/px -> not
        Map map = Map::open({{board, near}});
        auto img = CairoImage::create(ImageSize{40, 40}, error);
        img->canvas().clear(Color::fromHex(0xFFFFFF));
        MapReport rep = map.render(img->canvas(), view);
        CHECK(rep.warnings.empty());
        CHECK(channel(*img, 5, 5, 2) == 255 && channel(*img, 5, 5, 1) == 200);
        rep = map.render(img->canvas(), *makeView({0.0, 0.0, 2000.0, 2000.0}, ImageSize{40, 40}));
        CHECK(anyWarning(rep.warnings, "no rule of the style applies"));
    }
}

// §66: another CRS is refused (until step 4b-2); none is assumed to be the map's.
void testCrs(const std::string& board) {
    const std::string utm = writeBoard("r2d_raster_utm.tif", "EPSG:32631");
    Map map = Map::open({{board, {}}, {utm, {}}, {writeDsm(true), {}}});
    CHECK(map.layerCount() == 3);
    CHECK(map.crs().epsg == 31370);
    CHECK(anyWarning(map.warnings(), "a raster is not reprojected yet"));
    CHECK(anyWarning(map.warnings(), "r2d_raster_dsm: no CRS declared"));
    std::string error;
    auto img = CairoImage::create(ImageSize{4, 4}, error);
    const MapReport rep = map.render(img->canvas(), *makeView(kBoard, ImageSize{4, 4}));
    CHECK(rep.layers.size() == 2);   // the board and the DSM (elsewhere, nothing to draw)
    CHECK(anyWarning(rep.warnings, "r2d_raster_utm: not drawn"));
    // The map's CRS is the first DECLARED one, even under a raster without.
    Map below = Map::open({{writeDsm(true), {}}, {board, {}}});
    CHECK(below.crs().epsg == 31370);
}

} // namespace

int main() {
    const std::string board = writeBoard("r2d_raster_board.tif");
    testOpen(board);
    testExact(board);
    testEnlarged(board);
    testHalfCell(board);
    testReduced(board);
    testDsm();
    testOnMap(board);
    testCrs(board);
    return check::summary();
}
