// SPDX-License-Identifier: AGPL-3.0-or-later
// One raster (a grid of pixels on the ground), read through GDAL.
// Only the window of the view is read, at the size of the image; the file's
// overviews are used when they exist. See notions §65 and §66; in another
// system than the map's, it is warped on the fly (§72).
#pragma once

#include <memory>
#include <string>

#include "backend/gdal/gdal_layer.hpp"   // LayerInfo
#include "canvas/style.hpp"              // PixelBlock
#include "core/view.hpp"

namespace r2d {

// What read() gives: the pixels of the view's image that fall on the raster.
struct RasterRead {
    PixelBlock block;              // premultiplied ARGB32, ready for Canvas::drawPixels
    int        x = 0, y = 0;       // where the block's top-left pixel goes in the image
    double     fileWidth  = 0.0;   // the file's pixels it covers (columns, rows)...
    double     fileHeight = 0.0;
    bool       reduced = false;    // ...averaged into fewer pixels (else: nearest pixel)
    int        level = 0;          // §73: the overview read (0: the full resolution)...
    int        levels = 0;         // ...among so many
    double     levelPixel = 0.0;   // ...and its pixel size, in the units of the grid read
    double     ms = 0.0;           // reading and conversion
};

class GdalRaster {
public:
    // nullptr and `error` when it is not a raster GDAL can read, or when it
    // has no position on the ground (no geotransform), or a rotated one.
    // §73: a web service ("WMS:...", "WMTS:...", or GDAL's XML description
    // of one) is opened with a <Cache /> (its tiles kept on the disk), named
    // after its layer, and marked info().web.
    static std::unique_ptr<GdalRaster> open(const std::string& path, std::string& error);
    ~GdalRaster();

    GdalRaster(const GdalRaster&) = delete;
    GdalRaster& operator=(const GdalRaster&) = delete;

    const LayerInfo& info() const;        // raster = true, type empty, no feature
    int  width() const;                   // the file's pixels
    int  height() const;
    bool hasOverviews() const;

    // §72: shown in another system. The file is wrapped in a warped VRT, a
    // virtual image already in `to`: nothing is computed in advance, each
    // window read is warped on demand, from the file's overviews. `from` is
    // the file's system (declared in the map file, or its own). Bilinear,
    // or the nearest pixel for a palette (a mix of two classes means
    // nothing). False and `error` when GDAL cannot: the file is then shown
    // as it is. unwarp(): back to the file itself.
    // §73: a web service covers far more than the map's system was made
    // for (OpenStreetMap: the world). It is warped only over the area of use
    // of `to` (Lambert 72: Belgium), and its own extent when smaller.
    bool warpTo(const CrsInfo& from, const CrsInfo& to, std::string& error);
    void unwarp();
    bool warped() const;

    // Where it lies in the system it is shown in: info().extent while it is
    // not warped, the warped image's rectangle once it is.
    const Envelope& extent() const;

    // The view's pixels whose centre falls on the raster (§65). Nothing to
    // draw: an empty block. A GDAL error: an empty block and `error`.
    RasterRead read(const View& view, std::string& error);

    // §73: a web layer opened (or the time to open it: its description,
    // sometimes downloaded).
    double openMs() const;

private:
    GdalRaster();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// §73: GDAL's XML description of a web service, with a <Cache /> added when
// it has none (without it, GDAL keeps nothing on the disk, whatever the
// variables say: found on 2 November). Unchanged otherwise. For the tests.
std::string withWebCache(const std::string& xml);

// §73: a web layer's readable name, from its address: "Ortho2024NS" from
// "WMTS:...,layer=Ortho2024NS,...", the first of LAYERS= for "WMS:...".
// Empty when the source is not such an address.
std::string webLayerName(const std::string& source);

} // namespace r2d
