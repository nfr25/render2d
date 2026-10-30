// SPDX-License-Identifier: AGPL-3.0-or-later
// Reprojection on the fly (notions §69, §70): each layer stays in its own
// coordinate system; its features are converted to the map's as they are
// read. GDAL and PROJ stay in this folder: the map sees CrsInfo, Geometry
// and a FeatureSource.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "backend/gdal/gdal_layer.hpp"
#include "core/envelope.hpp"
#include "core/feature_source.hpp"
#include "core/geometry.hpp"

namespace r2d {

// The map's CRS from what the user typed: "EPSG:3857", a WKT, a .prj file...
// Refused (nullopt, `error`) when unknown, or in degrees (§70: the map must
// be in metres).
std::optional<CrsInfo> crsFromUser(const std::string& text, std::string& error);

// Any CRS, in degrees too: "EPSG:4326", a WKT (a layer's, as editor2d keeps
// it). nullopt and `error` when GDAL does not know it.
std::optional<CrsInfo> crsFromText(const std::string& text, std::string& error);

// EPSG:3857, for a map whose first layer is in degrees (§70).
CrsInfo webMercator();

// The map's CRS taken from a layer's: the same, or EPSG:3857 when it is in
// degrees (§70: a map in degrees is refused).
CrsInfo crsForMap(const CrsInfo& layerCrs);

// §70: GDAL gives every GeoJSON without "crs" the system EPSG:4326 (RFC
// 7946). A layer declared in degrees whose extent goes beyond -360..360 x
// -90..90 is not believed: false, and it is assumed to be in the map's CRS.
bool crsBelievable(const CrsInfo& crs, const std::optional<Envelope>& extent);

// A layer's extent in the map's CRS (editor2d's Zoom to layer): as it is
// when nothing needs converting (same CRS, none declared, not believable).
std::optional<Envelope> extentInCrs(const CrsInfo& layerCrs, const std::optional<Envelope>& extent,
                                    const CrsInfo& mapCrs);

// Nothing to convert: the same EPSG code, or the same definition. A CRS not
// declared counts as the same (assumed to be the map's, with a warning).
bool sameCrs(const CrsInfo& a, const CrsInfo& b);

// ---- The catalogue (editor2d's Map system dialog, its §70) ----------------------

// One projected system as PROJ's database lists it.
struct CrsEntry {
    std::string code;              // "EPSG:5316"
    std::string name;              // "ETRS89 / Faroe TM"
    std::string area;              // "Faroe Islands - onshore and offshore."
    bool        hasArea = false;   // the rectangle below is known
    double      west = 0, south = 0, east = 0, north = 0;   // its area of use, degrees

    // The point (degrees) is in the area's rectangle. One crossing the
    // 180th meridian has west > east.
    bool contains(double lon, double lat) const;
    // The rectangle's size, in square degrees: the local systems first.
    double size() const;
};

// Every projected system of the EPSG register that is not deprecated
// (about 5,000), read once from proj.db, sorted by code. Thread-safe.
const std::vector<CrsEntry>& projectedCatalog();

// The entries whose code, name and area together contain every word of
// `words` (any case: "faroe", "utm 29n", "31370"); with `near` (degrees),
// only those whose area of use contains it, the most local first.
// Otherwise by code. At most `limit` (0 = all).
std::vector<const CrsEntry*> searchCatalog(const std::vector<CrsEntry>& catalog, const std::string& words,
                                           const std::optional<Point>& near, std::size_t limit = 0);

// The catalogue's entry of a system (by its EPSG code), or nullptr.
const CrsEntry* catalogEntry(const CrsInfo& crs);

// A point of `crs` in degrees, longitude first (the map's centre, to look
// for the systems made for that place).
std::optional<Point> toLonLat(const CrsInfo& crs, Point p);

class CrsTransform;

// ---- Measuring (editor2d's Distance and Area tools, its §72) --------------------

struct Measurement {
    double length = 0.0;        // metres on the ground (the WGS 84 ellipsoid); closed: the perimeter
    double area   = 0.0;        // square metres on the ground (closed only)
    double mapLength = 0.0;     // the same in the map's units (planar)
    double mapArea   = 0.0;
    bool   onGround  = false;   // false: the map has no known system, only the map's figures
};

// Lengths and areas measured on the ellipsoid, whatever the map's system:
// the vertices to degrees, then Karney's geodesics (PROJ's geodesic.h). The
// conversion is prepared once; one meter per thread.
class GeodesicMeter {
public:
    explicit GeodesicMeter(const CrsInfo& mapCrs);
    ~GeodesicMeter();
    GeodesicMeter(const GeodesicMeter&) = delete;
    GeodesicMeter& operator=(const GeodesicMeter&) = delete;

    // `closed`: a polygon (perimeter and area; the closing side is implied),
    // else a broken line (its length).
    Measurement measure(const std::vector<Point>& points, bool closed) const;

private:
    std::unique_ptr<CrsTransform> toDegrees_;
};

// One way, from a layer's CRS to the map's. Not thread-safe: one per Map,
// as the open layers (§46).
class CrsTransform {
public:
    // nullptr and `error` when PROJ knows no way between the two.
    // `near`: a rectangle of the layer (its extent) where PROJ's choice is
    // looked at, for description(); may be empty.
    static std::unique_ptr<CrsTransform> create(const CrsInfo& from, const CrsInfo& to,
                                                const std::optional<Envelope>& near, std::string& error);
    ~CrsTransform();
    CrsTransform(const CrsTransform&) = delete;
    CrsTransform& operator=(const CrsTransform&) = delete;

    // Every vertex, in place, in one call. False if one cannot be converted:
    // the geometry is then unusable.
    bool forward(Geometry& g) const;

    // A rectangle of the map, as a rectangle of the layer: 21 points per edge
    // (a straight edge here is a curve there). Empty when it cannot be.
    std::optional<Envelope> backwardBounds(const Envelope& mapRect) const;

    // A rectangle of the layer, as a rectangle of the map (its extent).
    std::optional<Envelope> forwardBounds(const Envelope& layerRect) const;

    // "EPSG:4326 -> EPSG:31370 by Inverse of BD72 to WGS 84 (3) + Belgian Lambert 72 (1 m)"
    const std::string& description() const;

private:
    CrsTransform();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A GDAL layer seen in the map's CRS (§70): the spatial filter converted to
// the layer's CRS, each geometry converted to the map's. renderLayer and the
// queries use it as they use the layer itself.
class ReprojectedLayer final : public FeatureSource {
public:
    ReprojectedLayer(GdalLayer& layer, const CrsTransform& toMap) : layer_(layer), toMap_(toMap) {}

    const std::vector<FieldDef>& fields() const override { return layer_.fields(); }
    void setNeededFields(const std::vector<int>& columns) override { layer_.setNeededFields(columns); }
    void setSpatialFilter(const Envelope& mapRect) override;
    ReadStats forEach(const FeatureVisitor& visit) override;

    void      clearSpatialFilter();
    ReadStats forFids(const std::vector<FeatureId>& fids, const FeatureVisitor& visit);

private:
    // The visitor that converts, then hands over (or counts `outside`).
    FeatureVisitor converting(const FeatureVisitor& visit, std::size_t& outside);

    GdalLayer&          layer_;
    const CrsTransform& toMap_;
    Geometry            converted_;          // reused, as GdalLayer reuses its own
    bool                nothing_ = false;    // the filter's rectangle has no image in the layer's CRS
};

} // namespace r2d
