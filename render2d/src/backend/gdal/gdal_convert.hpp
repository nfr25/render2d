// SPDX-License-Identifier: AGPL-3.0-or-later
// OGR geometry -> flat Geometry, and what the vector and raster readers
// share. Internal to backend/gdal. notions §16, §65.
#pragma once

#include "backend/gdal/gdal_layer.hpp"   // CrsInfo
#include "core/geometry.hpp"

#include <string>

class OGRGeometry;
class OGRSpatialReference;

namespace r2d {

enum class ConvertResult { Ok, Empty, Unsupported };

// Fills `out` (cleared first). Z and M are ignored, curves are linearized.
ConvertResult toGeometry(const OGRGeometry* g, Geometry& out);

// Once per program: register the drivers, keep GDAL quiet on stderr.
void gdalInit();

// The name and EPSG code of a CRS (declared, or guessed from a .prj), its
// WKT and whether it is in degrees.
CrsInfo crsInfo(const OGRSpatialReference* srs);

// The PROJ folder found beside the executable (§68), or "" (PROJ's default,
// or PROJ_DATA). For a PROJ context created outside GDAL (§70).
const std::string& projDataFolder();

} // namespace r2d
