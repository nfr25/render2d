// SPDX-License-Identifier: AGPL-3.0-or-later
#include "backend/gdal/gdal_reproject.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <sstream>

#include <cpl_conv.h>
#include <ogr_spatialref.h>
#include <proj.h>
#include <geodesic.h>

#include "backend/gdal/gdal_convert.hpp"

namespace r2d {

namespace {

// A CRS from its WKT, x first (§69: longitude, then latitude, as in the files).
bool fromWkt(const CrsInfo& c, OGRSpatialReference& srs) {
    if (c.wkt.empty() || srs.importFromWkt(c.wkt.c_str()) != OGRERR_NONE) return false;
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    return true;
}

std::string shortLabel(const CrsInfo& c) {
    if (c.epsg != 0) return "EPSG:" + std::to_string(c.epsg);
    if (!c.code.empty()) return c.code;   // "OGC:CRS84"
    return c.name.empty() ? std::string("?") : c.name;
}

// The way PROJ takes near the layer (§69: "BD72 to WGS 84 (3)", 1 m). Asked
// to PROJ itself, which GDAL does not tell: one point converted at the
// centre of `near`, then the operation used for it.
std::string chosenOperation(const CrsInfo& from, const CrsInfo& to, const std::optional<Envelope>& near) {
    PJ_CONTEXT* ctx = proj_context_create();
    if (!ctx) return {};
    if (const std::string& dir = projDataFolder(); !dir.empty()) {
        const char* paths[] = {dir.c_str()};
        proj_context_set_search_paths(ctx, 1, paths);
    }
    std::string out;
    PJ* src = proj_create(ctx, from.wkt.c_str());
    PJ* dst = proj_create(ctx, to.wkt.c_str());
    PJ* op  = src && dst ? proj_create_crs_to_crs_from_pj(ctx, src, dst, nullptr, nullptr) : nullptr;
    PJ* norm = op ? proj_normalize_for_visualization(ctx, op) : nullptr;   // x = longitude first
    if (norm) {
        if (near) {
            PJ_COORD c = proj_coord(near->center().x, near->center().y, 0, 0);
            proj_trans(norm, PJ_FWD, c);
            if (PJ* used = proj_trans_get_last_used_operation(norm)) {
                if (const char* name = proj_get_name(used)) out = name;
                const double acc = proj_coordoperation_get_accuracy(ctx, used);
                if (acc >= 0) {
                    char text[32];
                    std::snprintf(text, sizeof text, " (%g m)", acc);
                    out += text;
                }
                proj_destroy(used);
            }
        }
        if (out.empty())
            if (const char* name = proj_get_name(norm)) out = name;
        // The x-first normalization adds its own step: not a choice of PROJ's.
        const std::string axis = "axis order change (2D) + ";
        if (out.rfind(axis, 0) == 0) out.erase(0, axis.size());
    }
    proj_destroy(norm);
    proj_destroy(op);
    proj_destroy(dst);
    proj_destroy(src);
    proj_context_destroy(ctx);
    return out;
}

} // namespace

// ---- the catalogue --------------------------------------------------------------

bool CrsEntry::contains(double lon, double lat) const {
    if (!hasArea || lat < south || lat > north) return false;
    return west <= east ? (lon >= west && lon <= east) : (lon >= west || lon <= east);
}

double CrsEntry::size() const {
    if (!hasArea) return 1e9;
    const double w = west <= east ? east - west : 360.0 - (west - east);
    return w * (north - south);
}

namespace {

int codeNumber(const std::string& code) {
    const auto colon = code.find(':');
    return colon == std::string::npos ? 0 : std::atoi(code.c_str() + colon + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<CrsEntry> readCatalog() {
    std::vector<CrsEntry> out;
    PJ_CONTEXT* ctx = proj_context_create();
    if (!ctx) return out;
    if (const std::string& dir = projDataFolder(); !dir.empty()) {
        const char* paths[] = {dir.c_str()};
        proj_context_set_search_paths(ctx, 1, paths);
    }
    PROJ_CRS_LIST_PARAMETERS* params = proj_get_crs_list_parameters_create();
    PJ_TYPE type = PJ_TYPE_PROJECTED_CRS;
    params->types            = &type;
    params->typesCount       = 1;
    params->allow_deprecated = 0;
    int n = 0;
    PROJ_CRS_INFO** list = proj_get_crs_info_list_from_database(ctx, "EPSG", params, &n);
    for (int i = 0; list && i < n; ++i) {
        const PROJ_CRS_INFO* c = list[i];
        CrsEntry e;
        e.code    = std::string(c->auth_name) + ":" + c->code;
        e.name    = c->name ? c->name : "";
        e.area    = c->area_name ? c->area_name : "";
        e.hasArea = c->bbox_valid != 0;
        e.west    = c->west_lon_degree;
        e.south   = c->south_lat_degree;
        e.east    = c->east_lon_degree;
        e.north   = c->north_lat_degree;
        out.push_back(std::move(e));
    }
    proj_crs_info_list_destroy(list);
    proj_get_crs_list_parameters_destroy(params);
    proj_context_destroy(ctx);
    std::sort(out.begin(), out.end(),
              [](const CrsEntry& a, const CrsEntry& b) { return codeNumber(a.code) < codeNumber(b.code); });
    return out;
}

} // namespace

const std::vector<CrsEntry>& projectedCatalog() {
    static std::once_flag once;
    static std::vector<CrsEntry> catalog;
    std::call_once(once, [] {
        gdalInit();   // PROJ's folder beside the executable (§68)
        catalog = readCatalog();
    });
    return catalog;
}

std::vector<const CrsEntry*> searchCatalog(const std::vector<CrsEntry>& catalog, const std::string& words,
                                           const std::optional<Point>& near, std::size_t limit) {
    std::vector<std::string> wanted;
    std::istringstream in(lower(words));
    for (std::string w; in >> w;) wanted.push_back(w);

    std::vector<const CrsEntry*> out;
    for (const CrsEntry& e : catalog) {
        if (near && !e.contains(near->x, near->y)) continue;
        const std::string text = lower(e.code + " " + e.name + " " + e.area);
        bool all = true;
        for (const std::string& w : wanted)
            if (text.find(w) == std::string::npos) { all = false; break; }
        if (all) out.push_back(&e);
    }
    if (near)   // the systems made for this place first: the smallest areas
        std::stable_sort(out.begin(), out.end(), [](const CrsEntry* a, const CrsEntry* b) { return a->size() < b->size(); });
    if (limit && out.size() > limit) out.resize(limit);
    return out;
}

const CrsEntry* catalogEntry(const CrsInfo& crs) {
    if (crs.epsg == 0) return nullptr;
    const std::string code = "EPSG:" + std::to_string(crs.epsg);
    for (const CrsEntry& e : projectedCatalog())
        if (e.code == code) return &e;
    return nullptr;
}

std::optional<Point> toLonLat(const CrsInfo& crs, Point p) {
    std::string error;
    const std::optional<CrsInfo> wgs84 = crsFromText("EPSG:4326", error);
    if (!wgs84 || !crs.known) return std::nullopt;
    if (sameCrs(crs, *wgs84)) return p;
    const auto t = CrsTransform::create(crs, *wgs84, std::nullopt, error);
    Geometry g;
    g.type = GeomType::Point;
    g.points.push_back(p);
    g.closePart(0);
    if (!t || !t->forward(g)) return std::nullopt;
    return g.points[0];
}

std::optional<CrsInfo> crsFromText(const std::string& text, std::string& error) {
    gdalInit();
    OGRSpatialReference srs;
    if (text.empty() || srs.SetFromUserInput(text.c_str()) != OGRERR_NONE) {
        error = "unknown coordinate system '" + text.substr(0, 60) + "' (try EPSG:31370, EPSG:3857...)";
        return std::nullopt;
    }
    return crsInfo(&srs);
}

std::optional<CrsInfo> crsFromUser(const std::string& text, std::string& error) {
    std::optional<CrsInfo> c = crsFromText(text, error);
    if (!c) return std::nullopt;
    if (c->geographic) {
        error = "'" + text + "' is in degrees: the map must be in metres (EPSG:3857, a UTM zone, EPSG:31370...)";
        return std::nullopt;
    }
    return c;
}

CrsInfo webMercator() {
    std::string error;
    return *crsFromUser("EPSG:3857", error);
}

CrsInfo crsForMap(const CrsInfo& layerCrs) {
    return layerCrs.geographic ? webMercator() : layerCrs;
}

bool crsBelievable(const CrsInfo& crs, const std::optional<Envelope>& extent) {
    if (!crs.known || !crs.geographic || !extent) return true;
    const Envelope& e = *extent;
    return !(e.xmin < -360.0 || e.xmax > 360.0 || e.ymin < -90.0 || e.ymax > 90.0);
}

std::optional<Envelope> extentInCrs(const CrsInfo& layerCrs, const std::optional<Envelope>& extent,
                                    const CrsInfo& mapCrs) {
    if (!extent || !layerCrs.known || !mapCrs.known || !crsBelievable(layerCrs, extent) ||
        sameCrs(layerCrs, mapCrs))
        return extent;
    std::string error;
    const auto t = CrsTransform::create(layerCrs, mapCrs, std::nullopt, error);
    return t ? t->forwardBounds(*extent) : std::nullopt;
}

bool sameCrs(const CrsInfo& a, const CrsInfo& b) {
    if (!a.known || !b.known) return true;
    if (a.epsg != 0 && a.epsg == b.epsg) return true;
    OGRSpatialReference sa, sb;
    if (!fromWkt(a, sa) || !fromWkt(b, sb)) return a.epsg == b.epsg;   // no WKT: the codes decide
    const char* options[] = {"IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES", "CRITERION=EQUIVALENT", nullptr};
    return sa.IsSame(&sb, options);
}

// ---- CrsTransform ----------------------------------------------------------------

struct CrsTransform::Impl {
    std::unique_ptr<OGRCoordinateTransformation> forward, backward;
    std::string                                  description;
    mutable std::vector<double>                  xs, ys;      // forward()'s arrays, reused
    mutable std::vector<int>                     ok;
};

CrsTransform::CrsTransform() : impl_(std::make_unique<Impl>()) {}
CrsTransform::~CrsTransform() = default;

std::unique_ptr<CrsTransform> CrsTransform::create(const CrsInfo& from, const CrsInfo& to,
                                                   const std::optional<Envelope>& near, std::string& error) {
    gdalInit();
    OGRSpatialReference src, dst;
    if (!fromWkt(from, src) || !fromWkt(to, dst)) {
        error = "cannot read the definition of " + shortLabel(from) + " or " + shortLabel(to);
        return nullptr;
    }
    std::unique_ptr<CrsTransform> t(new CrsTransform());
    t->impl_->forward.reset(OGRCreateCoordinateTransformation(&src, &dst));
    t->impl_->backward.reset(OGRCreateCoordinateTransformation(&dst, &src));
    if (!t->impl_->forward || !t->impl_->backward) {
        error = "no conversion known from " + shortLabel(from) + " to " + shortLabel(to);
        return nullptr;
    }
    t->impl_->description = shortLabel(from) + " -> " + shortLabel(to);
    if (const std::string op = chosenOperation(from, to, near); !op.empty())
        t->impl_->description += " by " + op;
    return t;
}

bool CrsTransform::forward(Geometry& g) const {
    const std::size_t n = g.points.size();
    if (n == 0) return true;
    auto& xs = impl_->xs;
    auto& ys = impl_->ys;
    auto& ok = impl_->ok;
    xs.resize(n);
    ys.resize(n);
    ok.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        xs[i] = g.points[i].x;
        ys[i] = g.points[i].y;
    }
    impl_->forward->Transform(n, xs.data(), ys.data(), nullptr, nullptr, ok.data());
    for (std::size_t i = 0; i < n; ++i) {
        if (!ok[i]) return false;
        g.points[i] = {xs[i], ys[i]};
    }
    return true;
}

namespace {

std::optional<Envelope> bounds(OGRCoordinateTransformation& ct, const Envelope& e) {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!ct.TransformBounds(e.xmin, e.ymin, e.xmax, e.ymax, &x0, &y0, &x1, &y1, 21)) return std::nullopt;
    const Envelope out{x0, y0, x1, y1};
    if (!out.isValid()) return std::nullopt;
    return out;
}

} // namespace

std::optional<Envelope> CrsTransform::backwardBounds(const Envelope& mapRect) const {
    return bounds(*impl_->backward, mapRect);
}

std::optional<Envelope> CrsTransform::forwardBounds(const Envelope& layerRect) const {
    return bounds(*impl_->forward, layerRect);
}

const std::string& CrsTransform::description() const { return impl_->description; }

// ---- GeodesicMeter ----------------------------------------------------------------

GeodesicMeter::GeodesicMeter(const CrsInfo& mapCrs) {
    std::string error;
    const std::optional<CrsInfo> wgs84 = crsFromText("EPSG:4326", error);
    if (wgs84 && mapCrs.known) toDegrees_ = CrsTransform::create(mapCrs, *wgs84, std::nullopt, error);
}

GeodesicMeter::~GeodesicMeter() = default;

Measurement GeodesicMeter::measure(const std::vector<Point>& points, bool closed) const {
    Measurement m;
    const std::size_t n = points.size();
    if (n < 2) return m;

    // The map's own figures: Pythagoras and the shoelace.
    for (std::size_t i = 1; i < n; ++i)
        m.mapLength += std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
    if (closed) {
        m.mapLength += std::hypot(points[0].x - points[n - 1].x, points[0].y - points[n - 1].y);
        double twice = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const Point& a = points[i];
            const Point& b = points[(i + 1) % n];
            twice += a.x * b.y - b.x * a.y;
        }
        m.mapArea = std::abs(twice) / 2.0;
    }
    if (!toDegrees_) return m;

    // On the ground: in degrees (longitude first), then on the ellipsoid.
    Geometry g;
    g.type   = GeomType::Line;
    g.points = points;
    g.closePart(0);
    if (!toDegrees_->forward(g)) return m;
    geod_geodesic e;
    geod_init(&e, 6378137.0, 1.0 / 298.257223563);   // WGS 84
    if (closed && n >= 3) {
        std::vector<double> lats(n), lons(n);
        for (std::size_t i = 0; i < n; ++i) {
            lons[i] = g.points[i].x;
            lats[i] = g.points[i].y;
        }
        double area = 0.0, perimeter = 0.0;
        geod_polygonarea(&e, lats.data(), lons.data(), static_cast<int>(n), &area, &perimeter);
        m.area   = std::abs(area);   // the sign is the turning direction
        m.length = perimeter;
    } else {
        for (std::size_t i = 1; i < n; ++i) {
            double s = 0.0;
            geod_inverse(&e, g.points[i - 1].y, g.points[i - 1].x, g.points[i].y, g.points[i].x, &s, nullptr, nullptr);
            m.length += s;
        }
    }
    m.onGround = true;
    return m;
}

// ---- ReprojectedLayer --------------------------------------------------------------

void ReprojectedLayer::setSpatialFilter(const Envelope& mapRect) {
    const std::optional<Envelope> r = toMap_.backwardBounds(mapRect);
    nothing_ = !r;
    if (!r) return;
    // A thousandth of the rectangle more on each side: the margin of rounding.
    layer_.setSpatialFilter(r->expandedBy(std::max(r->width(), r->height()) * 1e-3));
}

void ReprojectedLayer::clearSpatialFilter() {
    nothing_ = false;
    layer_.clearSpatialFilter();
}

FeatureVisitor ReprojectedLayer::converting(const FeatureVisitor& visit, std::size_t& outside) {
    return [this, &visit, &outside](const Geometry& g, const Attributes& a, FeatureId fid) {
        converted_.type   = g.type;
        converted_.points = g.points;   // keeps the memory: one copy per feature, no allocation
        converted_.parts  = g.parts;
        if (!toMap_.forward(converted_)) {
            ++outside;
            return;
        }
        visit(converted_, a, fid);
    };
}

ReadStats ReprojectedLayer::forEach(const FeatureVisitor& visit) {
    if (nothing_) return {};
    std::size_t outside = 0;
    ReadStats st = layer_.forEach(converting(visit, outside));
    st.drawn -= outside;
    st.outside += outside;
    return st;
}

ReadStats ReprojectedLayer::forFids(const std::vector<FeatureId>& fids, const FeatureVisitor& visit) {
    std::size_t outside = 0;
    ReadStats st = layer_.forFids(fids, converting(visit, outside));
    st.drawn -= outside;
    st.outside += outside;
    return st;
}

} // namespace r2d
