// SPDX-License-Identifier: AGPL-3.0-or-later
// One raster (a grid of pixels on the ground), read through GDAL.
// Only the window of the view is read, at the size of the image; the file's
// overviews are used when they exist. See notions §65 and §66.
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
    double     ms = 0.0;           // reading and conversion
};

class GdalRaster {
public:
    // nullptr and `error` when it is not a raster GDAL can read, or when it
    // has no position on the ground (no geotransform), or a rotated one.
    static std::unique_ptr<GdalRaster> open(const std::string& path, std::string& error);
    ~GdalRaster();

    GdalRaster(const GdalRaster&) = delete;
    GdalRaster& operator=(const GdalRaster&) = delete;

    const LayerInfo& info() const;        // raster = true, type empty, no feature
    int  width() const;                   // the file's pixels
    int  height() const;
    bool hasOverviews() const;

    // The view's pixels whose centre falls on the raster (§65). Nothing to
    // draw: an empty block. A GDAL error: an empty block and `error`.
    RasterRead read(const View& view, std::string& error);

private:
    GdalRaster();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace r2d
