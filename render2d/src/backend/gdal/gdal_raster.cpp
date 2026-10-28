// SPDX-License-Identifier: AGPL-3.0-or-later
#include "backend/gdal/gdal_raster.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal_priv.h>

#include "backend/gdal/gdal_convert.hpp"

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

} // namespace

struct GdalRaster::Impl {
    GDALDatasetUniquePtr dataset;
    LayerInfo            info;
    double               gt[6] = {0, 1, 0, 0, 0, 1};   // the geotransform (§65)
    int                  width = 0, height = 0;
    Kind                 kind = Kind::Grey;
    std::vector<Channel> channels;                     // 1 (grey, palette) or 3 (R, G, B)
    GDALRasterBand*      mask = nullptr;               // nodata, alpha band or mask; null = all valid
    bool                 overviews = false;
};

GdalRaster::GdalRaster() : impl_(std::make_unique<Impl>()) {}
GdalRaster::~GdalRaster() = default;

std::unique_ptr<GdalRaster> GdalRaster::open(const std::string& path, std::string& error) {
    gdalInit();
    CPLErrorReset();

    std::unique_ptr<GdalRaster> r(new GdalRaster());
    Impl& im = *r->impl_;
    im.dataset.reset(GDALDataset::Open(withSessionPassword(path).c_str(), GDAL_OF_RASTER));
    if (!im.dataset || im.dataset->GetRasterCount() == 0) {
        error = "cannot open '" + path + "' as a raster";
        if (CPLGetLastErrorMsg()[0] != '\0') error += std::string(": ") + CPLGetLastErrorMsg();
        return nullptr;
    }
    GDALDataset& ds = *im.dataset;
    im.width  = ds.GetRasterXSize();
    im.height = ds.GetRasterYSize();

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

    // 3. What is transparent: GDAL's mask covers nodata, an alpha band and an
    //    internal mask alike (255 = seen, 0 = transparent).
    const int flags = b1->GetMaskFlags();
    if (!(flags & GMF_ALL_VALID)) im.mask = b1->GetMaskBand();

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
    info.name   = CPLGetBasename(path.c_str());
    info.raster = true;
    info.crs    = crsInfo(ds.GetSpatialRef());
    const double xa = im.gt[0], xb = im.gt[0] + im.width * im.gt[1];
    const double ya = im.gt[3], yb = im.gt[3] + im.height * im.gt[5];
    info.extent = Envelope{std::min(xa, xb), std::min(ya, yb), std::max(xa, xb), std::max(ya, yb)};
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
int  GdalRaster::width() const { return impl_->width; }
int  GdalRaster::height() const { return impl_->height; }
bool GdalRaster::hasOverviews() const { return impl_->overviews; }

RasterRead GdalRaster::read(const View& view, std::string& error) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    RasterRead out;
    const Impl& im = *impl_;
    const double* g = im.gt;

    // 1. The raster's corners in image pixels; the image pixels whose CENTRE
    //    falls on the raster (two raster tiles side by side: each pixel
    //    belongs to one of them, no gap, no overlap).
    const Envelope& e = *im.info.extent;
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
