// SPDX-License-Identifier: AGPL-3.0-or-later
#include "backend/gdal/gdal_raster.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <ogr_spatialref.h>

#include "backend/gdal/gdal_convert.hpp"
#include "backend/gdal/gdal_reproject.hpp"

namespace r2d {

namespace {

// How the bands become colours (§66).
enum class Kind { Grey, Palette, Rgb };

// One band that gives a colour, and how its values are stretched to 0..255.
struct Channel {
    GDALRasterBand* band = nullptr;
    double          min = 0.0, max = 255.0;
};

// 0..255 from a value: min gives 0, max gives 255. A flat band: middle grey.
int stretch(double v, const Channel& ch) {
    if (!(ch.max > ch.min)) return 128;
    const double t = std::clamp((v - ch.min) / (ch.max - ch.min), 0.0, 1.0);
    return static_cast<int>(std::lround(t * 255.0));
}

// Colour and alpha (0..255, straight) -> premultiplied ARGB32 (§48, §66).
std::uint32_t premultiplied(int r, int g, int b, int a) {
    if (a <= 0) return 0;
    if (a < 255) {
        r = (r * a + 127) / 255;
        g = (g * a + 127) / 255;
        b = (b * a + 127) / 255;
    }
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
}

// "13" or "99.56": a short readable number.
std::string shortNumber(double x) {
    char s[32];
    std::snprintf(s, sizeof s, "%.4g", x);
    return s;
}

bool startsWithNoCase(const std::string& s, const char* prefix) {
    for (std::size_t i = 0; prefix[i]; ++i)
        if (i >= s.size() || std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

// §73: what to give GDAL for a web service, and what to call it.
struct WebSource {
    std::string open;          // the address, or the XML description itself
    bool        web = false;
    std::string name;          // empty: the usual name (the file's)
};

// GDAL's XML description of the service a "WMS:" address opens: what
// `gdal_translate -of WMS` writes, here in memory (/vsimem/).
std::string wmsDescription(const std::string& address) {
    GDALDatasetUniquePtr ds(GDALDataset::Open(address.c_str(), GDAL_OF_RASTER));
    GDALDriver* wms = GetGDALDriverManager()->GetDriverByName("WMS");
    if (!ds || !wms) return {};
    static int counter = 0;
    const std::string mem = "/vsimem/r2d_wms_" + std::to_string(++counter) + ".xml";
    GDALDatasetUniquePtr copy(wms->CreateCopy(mem.c_str(), ds.get(), FALSE, nullptr, nullptr, nullptr));
    copy.reset();
    std::string xml;
    vsi_l_offset size = 0;
    if (GByte* bytes = VSIGetMemFileBuffer(mem.c_str(), &size, FALSE))
        xml.assign(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(size));
    VSIUnlink(mem.c_str());
    return xml;
}

WebSource webSource(const std::string& path) {
    WebSource w;
    w.open = path;
    w.name = webLayerName(path);
    if (startsWithNoCase(path, "WMTS:")) {   // the WMTS driver keeps a cache by itself (seen 2 November)
        w.web = true;
        return w;
    }
    if (startsWithNoCase(path, "WMS:")) {    // opened from an address: no <Cache />, so a description with one
        w.web = true;
        const std::string xml = wmsDescription(path);
        if (!xml.empty()) w.open = withWebCache(xml);
        return w;
    }
    std::string text;
    if (startsWithNoCase(path, "<GDAL_WMS") || startsWithNoCase(path, "<GDAL_WMTS")) {
        text = path;
    } else if (path.size() > 4 && startsWithNoCase(path.substr(path.size() - 4), ".xml")) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream all;
        if (in) all << in.rdbuf();
        text = all.str();
        if (text.size() > (1u << 20)) text.clear();   // a description is small
    }
    if (text.find("<GDAL_WMS") != std::string::npos || text.find("<GDAL_WMTS") != std::string::npos) {
        w.web  = true;
        w.open = withWebCache(text);
    }
    return w;
}

// §73: where a web layer may be warped to `to`: its area of use (PROJ knows
// it for each system, in degrees), and the layer's own extent when smaller.
// Empty: no limit known.
std::optional<Envelope> webClip(const CrsInfo& from, const OGRSpatialReference* fileSrs, const CrsInfo& to,
                                const Envelope& extent) {
    const auto aoi = areaOfUse(to);
    if (!aoi) return std::nullopt;
    const Envelope& area = *aoi;
    OGRSpatialReference target;
    if (target.importFromWkt(to.wkt.c_str()) != OGRERR_NONE) return area;
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    // The layer's own extent, when it converts sensibly (the UrbIS WMS:
    // Belgium; OpenStreetMap: the world, which does not).
    OGRSpatialReference source;
    bool haveSource = false;
    if (!from.wkt.empty()) haveSource = source.importFromWkt(from.wkt.c_str()) == OGRERR_NONE;
    else if (fileSrs) { source = *fileSrs; haveSource = true; }
    if (haveSource) {
        source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        CPLErrorReset();
        std::unique_ptr<OGRCoordinateTransformation> own(OGRCreateCoordinateTransformation(&source, &target));
        Envelope e;
        if (own && own->TransformBounds(extent.xmin, extent.ymin, extent.xmax, extent.ymax,
                                        &e.xmin, &e.ymin, &e.xmax, &e.ymax, 21) &&
            std::isfinite(e.xmin) && std::isfinite(e.xmax) && std::isfinite(e.ymin) && std::isfinite(e.ymax)) {
            const Envelope both{std::max(e.xmin, area.xmin), std::max(e.ymin, area.ymin),
                                std::min(e.xmax, area.xmax), std::min(e.ymax, area.ymax)};
            if (both.xmax > both.xmin && both.ymax > both.ymin) return both;
        }
    }
    return area;
}

} // namespace

std::string withWebCache(const std::string& xml) {
    if (xml.find("<Cache") != std::string::npos) return xml;
    for (const char* end : {"</GDAL_WMS>", "</GDAL_WMTS>"}) {
        const std::size_t at = xml.rfind(end);
        if (at != std::string::npos) return xml.substr(0, at) + "  <Cache />\n" + xml.substr(at);
    }
    return xml;
}

std::string webLayerName(const std::string& source) {
    const char* key = startsWithNoCase(source, "WMTS:") ? "layer=" : startsWithNoCase(source, "WMS:") ? "layers=" : nullptr;
    if (!key) return {};
    std::string lower(source);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::size_t at = lower.find(key);
    while (at != std::string::npos && at > 0 && lower[at - 1] != ',' && lower[at - 1] != '&' && lower[at - 1] != '?')
        at = lower.find(key, at + 1);   // "tilematrixlayer=": not ours
    if (at == std::string::npos) return {};
    at += std::char_traits<char>::length(key);
    const std::size_t end = source.find_first_of(",&", at);
    return source.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

struct GdalRaster::Impl {
    GDALDatasetUniquePtr dataset;                      // the file
    GDALDatasetUniquePtr warpedDs;                     // §72: its warped VRT, or null. Closed first (declared after)
    LayerInfo            info;
    int                  fileWidth = 0, fileHeight = 0;
    Kind                 kind = Kind::Grey;
    bool                 overviews = false;
    double               openMs = 0.0;                 // §73
    // The grid read() works on: the file's, or the warped VRT's (§72).
    double               gt[6] = {0, 1, 0, 0, 0, 1};   // the geotransform (§65)
    int                  width = 0, height = 0;
    Envelope             extent;
    std::vector<Channel> channels;                     // 1 (grey, palette) or 3 (R, G, B)
    GDALRasterBand*      mask = nullptr;               // nodata, alpha band or mask; null = all valid

    // Reads the grid from `ds`: its geotransform, its bands, its mask. The
    // stretch (min, max) stays the file's: a warped image shows the same values.
    void useGrid(GDALDataset& ds) {
        ds.GetGeoTransform(gt);
        width  = ds.GetRasterXSize();
        height = ds.GetRasterYSize();
        for (std::size_t k = 0; k < channels.size(); ++k) channels[k].band = ds.GetRasterBand(static_cast<int>(k) + 1);
        // GDAL's mask covers nodata, an alpha band and an internal mask alike
        // (255 = seen, 0 = transparent): the warped VRT's alpha band too.
        GDALRasterBand* b1 = ds.GetRasterBand(1);
        mask = (b1->GetMaskFlags() & GMF_ALL_VALID) ? nullptr : b1->GetMaskBand();
        const double xa = gt[0], xb = gt[0] + width * gt[1];
        const double ya = gt[3], yb = gt[3] + height * gt[5];
        extent = Envelope{std::min(xa, xb), std::min(ya, yb), std::max(xa, xb), std::max(ya, yb)};
    }
};

GdalRaster::GdalRaster() : impl_(std::make_unique<Impl>()) {}
GdalRaster::~GdalRaster() = default;

std::unique_ptr<GdalRaster> GdalRaster::open(const std::string& path, std::string& error) {
    gdalInit();
    CPLErrorReset();

    std::unique_ptr<GdalRaster> r(new GdalRaster());
    Impl& im = *r->impl_;
    const auto t0 = std::chrono::steady_clock::now();
    const WebSource web = webSource(path);   // §73
    im.dataset.reset(GDALDataset::Open(web.web ? web.open.c_str() : withSessionPassword(path).c_str(), GDAL_OF_RASTER));
    im.openMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!im.dataset || im.dataset->GetRasterCount() == 0) {
        error = "cannot open '" + path + "' as a raster";
        if (CPLGetLastErrorMsg()[0] != '\0') error += std::string(": ") + CPLGetLastErrorMsg();
        return nullptr;
    }
    GDALDataset& ds = *im.dataset;
    im.width  = im.fileWidth  = ds.GetRasterXSize();
    im.height = im.fileHeight = ds.GetRasterYSize();

    // 1. Where it lies: the geotransform (from the file or its world file).
    if (ds.GetGeoTransform(im.gt) != CE_None) {
        error = "'" + path + "' has no position on the ground (no geotransform, no world file)";
        return nullptr;
    }
    if (im.gt[2] != 0.0 || im.gt[4] != 0.0) {
        error = "'" + path + "' is rotated (geotransform with rotation terms): not supported yet";
        return nullptr;
    }
    if (im.gt[1] == 0.0 || im.gt[5] == 0.0) {
        error = "'" + path + "' has a pixel size of 0";
        return nullptr;
    }

    // 2. What its bands are (§66).
    GDALRasterBand* b1 = ds.GetRasterBand(1);
    const int bands = ds.GetRasterCount();
    if (b1->GetColorTable() != nullptr && b1->GetColorInterpretation() == GCI_PaletteIndex) {
        im.kind = Kind::Palette;
        im.channels = {Channel{b1}};
    } else if (bands >= 3) {
        im.kind = Kind::Rgb;
        im.channels = {Channel{b1}, Channel{ds.GetRasterBand(2)}, Channel{ds.GetRasterBand(3)}};
    } else {
        im.kind = Kind::Grey;
        im.channels = {Channel{b1}};
    }
    im.overviews = b1->GetOverviewCount() > 0;

    // 3. The grid: the geotransform, the bands, what is transparent.
    const int flags = b1->GetMaskFlags();
    im.useGrid(ds);

    // 4. The stretch: min and max of the WHOLE raster, read once (§66). Bytes
    //    in colour are already 0..255. Approximate (from an overview, or a
    //    sample) when the file is big.
    const bool approx = im.overviews || static_cast<double>(im.width) * im.height > 64e6;
    for (Channel& ch : im.channels) {
        const bool byteColour = im.kind == Kind::Rgb && ch.band->GetRasterDataType() == GDT_Byte;
        if (im.kind == Kind::Palette || byteColour) continue;
        double minmax[2] = {0.0, 255.0};
        CPLErrorReset();
        if (ch.band->ComputeRasterMinMax(approx ? TRUE : FALSE, minmax) == CE_None) {
            ch.min = minmax[0];
            ch.max = minmax[1];
        }
    }

    // 5. The layer's description, as for a vector layer.
    LayerInfo& info = im.info;
    info.source = path;
    info.name   = web.name.empty() ? std::string(CPLGetBasename(path.c_str())) : web.name;
    const char* driver = ds.GetDriver() ? ds.GetDriver()->GetDescription() : "";
    info.web    = web.web || EQUAL(driver, "WMS") || EQUAL(driver, "WMTS");
    info.raster = true;
    info.crs    = crsInfo(ds.GetSpatialRef());
    info.extent = im.extent;
    switch (im.kind) {
        case Kind::Palette: info.rasterKind = "palette"; break;
        case Kind::Rgb:     info.rasterKind = (flags & GMF_ALPHA) ? "RGBA" : "RGB"; break;
        case Kind::Grey:
            info.rasterKind = "grey " + shortNumber(im.channels[0].min) + " to " + shortNumber(im.channels[0].max);
            break;
    }
    return r;
}

const LayerInfo& GdalRaster::info() const { return impl_->info; }
int  GdalRaster::width() const { return impl_->fileWidth; }
int  GdalRaster::height() const { return impl_->fileHeight; }
bool GdalRaster::hasOverviews() const { return impl_->overviews; }
const Envelope& GdalRaster::extent() const { return impl_->extent; }
bool GdalRaster::warped() const { return impl_->warpedDs != nullptr; }
double GdalRaster::openMs() const { return impl_->openMs; }

void GdalRaster::unwarp() {
    Impl& im = *impl_;
    if (!im.warpedDs) return;
    im.useGrid(*im.dataset);   // the bands first: they must not point into the VRT closed next
    im.warpedDs.reset();
}

// §72: what `gdalwarp -of VRT -t_srs <to> -r bilinear -dstalpha` does, in
// memory. -dstalpha: the corners of a turned image are transparent, not black.
bool GdalRaster::warpTo(const CrsInfo& from, const CrsInfo& to, std::string& error) {
    Impl& im = *impl_;
    unwarp();
    if (to.wkt.empty()) {
        error = "the map's system has no definition to warp to";
        return false;
    }
    CPLStringList args;
    for (const char* a : {"-q", "-of", "VRT", "-dstalpha", "-r"}) args.AddString(a);
    args.AddString(im.kind == Kind::Palette ? "near" : "bilinear");
    args.AddString("-t_srs");
    args.AddString(to.wkt.c_str());
    if (!from.wkt.empty()) {   // a declared system wins over the file's own (§70)
        args.AddString("-s_srs");
        args.AddString(from.wkt.c_str());
    }
    if (im.info.web) {   // §73: only where the map's system means something
        if (const auto clip = webClip(from, im.dataset->GetSpatialRef(), to, *im.info.extent)) {
            args.AddString("-te");
            for (double v : {clip->xmin, clip->ymin, clip->xmax, clip->ymax}) {
                char number[40];
                std::snprintf(number, sizeof number, "%.17g", v);
                args.AddString(number);
            }
        }
    }
    GDALWarpAppOptions* options = GDALWarpAppOptionsNew(args.List(), nullptr);
    GDALDatasetH source = GDALDataset::ToHandle(im.dataset.get());
    int usage = 0;
    CPLErrorReset();
    GDALDatasetH vrt = options ? GDALWarp("", nullptr, 1, &source, options, &usage) : nullptr;
    GDALWarpAppOptionsFree(options);
    if (!vrt) {
        error = im.info.name + ": cannot be warped to " + (to.code.empty() ? to.name : to.code);
        if (CPLGetLastErrorMsg()[0] != '\0') error += std::string(": ") + CPLGetLastErrorMsg();
        return false;
    }
    im.warpedDs.reset(GDALDataset::FromHandle(vrt));
    im.useGrid(*im.warpedDs);
    return true;
}

RasterRead GdalRaster::read(const View& view, std::string& error) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    RasterRead out;
    const Impl& im = *impl_;
    const double* g = im.gt;

    // 1. The raster's corners in image pixels; the image pixels whose CENTRE
    //    falls on the raster (two raster tiles side by side: each pixel
    //    belongs to one of them, no gap, no overlap).
    const Envelope& e = im.extent;   // the file's, or the warped image's (§72)
    const Point a = view.worldToImage.apply({e.xmin, e.ymax});
    const Point b = view.worldToImage.apply({e.xmax, e.ymin});
    auto first = [](double edge, int size) {   // first pixel whose centre is >= edge
        return static_cast<int>(std::clamp(std::ceil(edge - 0.5), 0.0, static_cast<double>(size)));
    };
    const int c0 = first(std::min(a.x, b.x), view.size.width),  c1 = first(std::max(a.x, b.x), view.size.width);
    const int r0 = first(std::min(a.y, b.y), view.size.height), r1 = first(std::max(a.y, b.y), view.size.height);
    if (c1 <= c0 || r1 <= r0) return out;
    const int n = c1 - c0, m = r1 - r0;

    // Image pixel -> file column / row (fractional). x depends on the column
    // only, y on the row only: views are not rotated.
    auto fileCol = [&](double px) { return (view.imageToWorld.apply({px, 0.0}).x - g[0]) / g[1]; };
    auto fileRow = [&](double py) { return (view.imageToWorld.apply({0.0, py}).y - g[3]) / g[5]; };
    const double kx = std::abs(fileCol(c0 + 1.0) - fileCol(c0));   // file pixels per image pixel
    const double ky = std::abs(fileRow(r0 + 1.0) - fileRow(r0));

    // 2. Read each band into floats. `colIdx`, `rowIdx`: where each image
    //    pixel finds its value in the buffer `bw` wide.
    std::vector<int> colIdx(n), rowIdx(m);
    int xOff = 0, yOff = 0, xSize = 0, ySize = 0, bw = 0, bh = 0;
    GDALRasterIOExtraArg arg;
    INIT_RASTERIO_EXTRA_ARG(arg);

    if (kx <= 1.0 && ky <= 1.0) {
        // Enlarged (or 1:1): the nearest file pixel, the one under the image
        // pixel's centre. Read the small window at full resolution.
        std::vector<int> cols(n), rows(m);
        for (int i = 0; i < n; ++i)
            cols[i] = std::clamp(static_cast<int>(std::floor(fileCol(c0 + i + 0.5))), 0, im.width - 1);
        for (int j = 0; j < m; ++j)
            rows[j] = std::clamp(static_cast<int>(std::floor(fileRow(r0 + j + 0.5))), 0, im.height - 1);
        const auto [cmin, cmax] = std::minmax_element(cols.begin(), cols.end());
        const auto [rmin, rmax] = std::minmax_element(rows.begin(), rows.end());
        xOff = *cmin; xSize = *cmax - *cmin + 1;
        yOff = *rmin; ySize = *rmax - *rmin + 1;
        bw = xSize; bh = ySize;
        for (int i = 0; i < n; ++i) colIdx[i] = cols[i] - xOff;
        for (int j = 0; j < m; ++j) rowIdx[j] = rows[j] - yOff;
    } else {
        // Reduced: GDAL averages the file pixels under each image pixel, from
        // the best overview. The window is fractional (§65): rounding it
        // would shift the image under the lines. Clamped to the file.
        double u0 = fileCol(c0), u1 = fileCol(c1), v0 = fileRow(r0), v1 = fileRow(r1);
        if (u0 > u1) std::swap(u0, u1);
        if (v0 > v1) std::swap(v0, v1);
        u0 = std::max(u0, 0.0); u1 = std::min(u1, static_cast<double>(im.width));
        v0 = std::max(v0, 0.0); v1 = std::min(v1, static_cast<double>(im.height));
        if (!(u1 > u0) || !(v1 > v0)) return out;
        arg.eResampleAlg = im.kind == Kind::Palette ? GRIORA_NearestNeighbour : GRIORA_Average;   // no average of palette indices
        arg.bFloatingPointWindowValidity = TRUE;
        arg.dfXOff = u0; arg.dfXSize = u1 - u0;
        arg.dfYOff = v0; arg.dfYSize = v1 - v0;
        xOff  = std::clamp(static_cast<int>(std::floor(u0)), 0, im.width - 1);
        yOff  = std::clamp(static_cast<int>(std::floor(v0)), 0, im.height - 1);
        xSize = std::clamp(static_cast<int>(std::ceil(u1)) - xOff, 1, im.width - xOff);
        ySize = std::clamp(static_cast<int>(std::ceil(v1)) - yOff, 1, im.height - yOff);
        bw = n; bh = m;
        const bool flipX = g[1] < 0.0, flipY = g[5] > 0.0;   // file columns or rows the other way
        for (int i = 0; i < n; ++i) colIdx[i] = flipX ? n - 1 - i : i;
        for (int j = 0; j < m; ++j) rowIdx[j] = flipY ? m - 1 - j : j;
        out.reduced = true;
    }
    // §73: which level GDAL reads: the coarsest overview at least as fine
    // as the view (GDAL's rule, without its small tolerance). For a web
    // service, the only meaningful figure.
    {
        GDALRasterBand* band = im.channels[0].band;
        out.levels = band->GetOverviewCount();
        double factor = 1.0;
        if (out.reduced) {
            const double wanted = std::min(kx, ky);
            for (int k = 0; k < out.levels; ++k) {
                GDALRasterBand* o = band->GetOverview(k);
                if (!o || o->GetXSize() <= 0) continue;
                const double f = static_cast<double>(im.width) / o->GetXSize();
                if (f <= wanted && f > factor) { factor = f; out.level = k + 1; }
            }
        }
        out.levelPixel = std::abs(g[1]) * factor;
    }
    out.fileWidth  = arg.bFloatingPointWindowValidity ? arg.dfXSize : xSize;
    out.fileHeight = arg.bFloatingPointWindowValidity ? arg.dfYSize : ySize;

    const std::size_t count = static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh);
    auto readBand = [&](GDALRasterBand* band, std::vector<float>& buf) {
        buf.resize(count);
        CPLErrorReset();
        if (band->RasterIO(GF_Read, xOff, yOff, xSize, ySize, buf.data(), bw, bh, GDT_Float32, 0, 0, &arg) == CE_None)
            return true;
        error = im.info.name + ": " + (CPLGetLastErrorMsg()[0] ? CPLGetLastErrorMsg() : "cannot read the pixels");
        return false;
    };
    std::vector<std::vector<float>> values(im.channels.size());
    for (std::size_t k = 0; k < im.channels.size(); ++k)
        if (!readBand(im.channels[k].band, values[k])) return out;
    std::vector<float> mask;
    if (im.mask && !readBand(im.mask, mask)) return out;

    // 3. Floats -> premultiplied ARGB32 (§66).
    const GDALColorTable* palette = im.kind == Kind::Palette ? im.channels[0].band->GetColorTable() : nullptr;
    const int paletteSize = palette ? palette->GetColorEntryCount() : 0;
    out.block.width  = n;
    out.block.height = m;
    out.block.argb.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(m), 0u);
    for (int j = 0; j < m; ++j) {
        std::uint32_t* row = out.block.argb.data() + static_cast<std::size_t>(j) * n;
        const std::size_t base = static_cast<std::size_t>(rowIdx[j]) * bw;
        for (int i = 0; i < n; ++i) {
            const std::size_t at = base + colIdx[i];
            int alpha = mask.empty() ? 255 : static_cast<int>(std::lround(std::clamp(mask[at], 0.0f, 255.0f)));
            if (alpha == 0) continue;
            const float v = values[0][at];
            if (!std::isfinite(v)) continue;
            int r = 0, gr = 0, bl = 0;
            switch (im.kind) {
                case Kind::Grey:
                    r = gr = bl = stretch(v, im.channels[0]);
                    break;
                case Kind::Palette: {
                    const int index = static_cast<int>(std::lround(v));
                    if (index < 0 || index >= paletteSize) continue;
                    const GDALColorEntry* c = palette->GetColorEntry(index);
                    r = c->c1; gr = c->c2; bl = c->c3;
                    alpha = alpha * std::clamp<int>(c->c4, 0, 255) / 255;
                    break;
                }
                case Kind::Rgb:
                    r  = stretch(v, im.channels[0]);
                    gr = stretch(values[1][at], im.channels[1]);
                    bl = stretch(values[2][at], im.channels[2]);
                    break;
            }
            row[i] = premultiplied(r, gr, bl, alpha);
        }
    }
    out.x  = c0;
    out.y  = r0;
    out.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
}

} // namespace r2d
