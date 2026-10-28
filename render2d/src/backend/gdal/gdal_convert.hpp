// SPDX-License-Identifier: AGPL-3.0-or-later
// OGR geometry -> flat Geometry, and what the vector and raster readers
// share. Internal to backend/gdal. notions §16, §65.
#pragma once

#include "backend/gdal/gdal_layer.hpp"   // CrsInfo
#include "core/geometry.hpp"

class OGRGeometry;
class OGRSpatialReference;

namespace r2d {

enum class ConvertResult { Ok, Empty, Unsupported };

// Fills `out` (cleared first). Z and M are ignored, curves are linearized.
ConvertResult toGeometry(const OGRGeometry* g, Geometry& out);

// Once per program: register the drivers, keep GDAL quiet on stderr.
void gdalInit();

// The name and EPSG code of a CRS (declared, or guessed from a .prj).
CrsInfo crsInfo(const OGRSpatialReference* srs);

} // namespace r2d
