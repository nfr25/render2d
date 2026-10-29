// SPDX-License-Identifier: AGPL-3.0-or-later
// One layer of a vector source, read through GDAL/OGR.
// This folder is the only place that includes GDAL headers: the rest of the
// renderer only sees Geometry. See notions §17 and §18.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/envelope.hpp"
#include "core/feature_source.hpp"
#include "core/geometry.hpp"

namespace r2d {

struct CrsInfo {
    bool        known = false;   // the source declares a CRS
    std::string name;            // e.g. "BD72 / Belgian Lambert 72"
    int         epsg  = 0;       // 0 when not identified
};

struct LayerInfo {
    std::string             source;
    std::string             name;
    std::optional<GeomType> type;          // empty when unknown or mixed
    long long               featureCount = -1;  // -1 when costly to know
    std::optional<Envelope> extent;
    CrsInfo                 crs;
    int                     layersInSource = 1;   // a GeoPackage may hold several (editor2d §39)
    bool                    raster = false;       // §65: a grid of pixels, no feature, no field
    bool                    hasGeometry = true;   // false: a table of attributes only (a GeoPackage's domains)
    std::string             rasterKind;           // §66: "RGB", "grey 13 to 99.56"...
};

// "3.13.2": for --version (the rest of the program does not include GDAL).
std::string gdalVersion();

// Where GDAL's and PROJ's reference files are read from (notions §68):
// "GDAL data: <folder> (beside the executable)", the same for PROJ; one line
// each, for --version. A distributed render2d carries them in share/gdal and
// share/proj next to the executable; an environment variable (GDAL_DATA,
// PROJ_DATA or PROJ_LIB) still wins.
std::vector<std::string> gdalDataInfo();

// A zone written in WKT (§56): "POLYGON((x y, x y, ...))", read by GDAL. The
// outer ring only; false and a message for anything else.
bool zoneFromWkt(const std::string& wkt, std::vector<Point>& ring, std::string& error);

// §67: a password typed for this session, kept in memory only. When GDAL
// opens `connection` ("PG:host=... dbname=...", without "::table"), the
// password is added to what GDAL receives; the source itself, the map file,
// the reports and the messages never contain it. Empty: forgotten.
// (Not an environment variable: on Windows, libpq.dll did not see the
// PGPASSWORD set by editor2d, 27 October.)
void setSessionPassword(const std::string& connection, const std::string& password);

// What GDAL is given for `source`: the source, plus " password='...'" when
// a session password is known for it. For the readers of this folder, and
// for the tests.
std::string withSessionPassword(const std::string& source);

// The vector layers of a source (a GeoPackage, a database): name, type,
// CRS, number of features when GDAL knows it cheaply (-1 otherwise: never a
// count of millions of rows), whether it has a geometry; no extent (costly
// on a big table). For a
// PostGIS connection, the tables with a geometry column ("schema.table"
// outside public). Empty and `error` when the source cannot be opened.
// editor2d's "Connect to database" (its §65).
std::vector<LayerInfo> listLayers(const std::string& source, std::string& error);

class GdalLayer final : public FeatureSource {
public:
    // `layerName` empty = first layer (the only one for a shapefile).
    // Returns nullptr and fills `error` when the source or layer cannot be opened.
    static std::unique_ptr<GdalLayer> open(const std::string& source,
                                           const std::string& layerName,
                                           std::string& error);
    ~GdalLayer() override;

    GdalLayer(const GdalLayer&) = delete;
    GdalLayer& operator=(const GdalLayer&) = delete;

    const LayerInfo& info() const;

    const std::vector<FieldDef>& fields() const override;

    // GDAL is told to ignore every other field (SetIgnoredFields). With no
    // field at all, a shapefile's .dbf is not needed. notions §30.
    void setNeededFields(const std::vector<int>& columns) override;

    // Only features touching this rectangle (in the layer's CRS) are read.
    void setSpatialFilter(const Envelope& e) override;

    // Reads the layer, one feature at a time. The Geometry and Attributes
    // passed to `visit` are reused for the next feature: copy them to keep them.
    ReadStats forEach(const FeatureVisitor& visit) override;

    // §64: no spatial filter any more: the whole layer (a selection by attributes).
    void clearSpatialFilter();

    // §64: only these features, by FID, in this order (the needed fields as
    // set). By GDAL's random access when the format has it, else one full read.
    ReadStats forFids(const std::vector<FeatureId>& fids, const FeatureVisitor& visit);

private:
    GdalLayer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace r2d
