// SPDX-License-Identifier: AGPL-3.0-or-later
#include "backend/gdal/gdal_convert.hpp"

#include <cstdlib>
#include <memory>
#include <mutex>

#include <cpl_error.h>
#include <gdal_priv.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>

namespace r2d {

namespace {

// Appends all the vertices of a line or ring as one part.
void addCurve(const OGRSimpleCurve& c, Geometry& out) {
    const std::size_t start = out.points.size();
    const int n = c.getNumPoints();
    for (int i = 0; i < n; ++i) {
        out.points.push_back({c.getX(i), c.getY(i)});
    }
    out.closePart(start);
}

// Exterior ring first, then the holes, each one a part. notions §16.
void addPolygon(const OGRPolygon& p, Geometry& out) {
    if (const OGRLinearRing* ext = p.getExteriorRing()) addCurve(*ext, out);
    for (int i = 0; i < p.getNumInteriorRings(); ++i) {
        addCurve(*p.getInteriorRing(i), out);
    }
}

void addPoint(const OGRPoint& p, Geometry& out) {
    const std::size_t start = out.points.size();
    out.points.push_back({p.getX(), p.getY()});
    out.closePart(start);
}

} // namespace

ConvertResult toGeometry(const OGRGeometry* g, Geometry& out) {
    out.clear();
    if (g == nullptr || g->IsEmpty()) return ConvertResult::Empty;

    // Arcs and curves (CircularString, CurvePolygon...): approximate them by
    // straight segments and convert the result. "Always a result".
    if (g->hasCurveGeometry()) {
        std::unique_ptr<OGRGeometry> lin(g->getLinearGeometry());
        if (!lin) return ConvertResult::Unsupported;
        return toGeometry(lin.get(), out);
    }

    switch (wkbFlatten(g->getGeometryType())) {
        case wkbPoint:
            out.type = GeomType::Point;
            addPoint(*g->toPoint(), out);
            break;
        case wkbMultiPoint: {
            out.type = GeomType::Point;
            const OGRMultiPoint* mp = g->toMultiPoint();
            for (int i = 0; i < mp->getNumGeometries(); ++i) addPoint(*mp->getGeometryRef(i), out);
            break;
        }
        case wkbLineString:
        case wkbLinearRing:
            out.type = GeomType::Line;
            addCurve(*g->toSimpleCurve(), out);
            break;
        case wkbMultiLineString: {
            out.type = GeomType::Line;
            const OGRMultiLineString* ml = g->toMultiLineString();
            for (int i = 0; i < ml->getNumGeometries(); ++i) addCurve(*ml->getGeometryRef(i), out);
            break;
        }
        case wkbPolygon:
            out.type = GeomType::Polygon;
            addPolygon(*g->toPolygon(), out);
            break;
        case wkbMultiPolygon: {
            out.type = GeomType::Polygon;
            const OGRMultiPolygon* mp = g->toMultiPolygon();
            for (int i = 0; i < mp->getNumGeometries(); ++i) addPolygon(*mp->getGeometryRef(i), out);
            break;
        }
        default:
            // GeometryCollection, TIN, polyhedral surfaces...: not drawn yet.
            return ConvertResult::Unsupported;
    }
    return out.empty() ? ConvertResult::Empty : ConvertResult::Ok;
}

// Once per program: register the drivers, and keep GDAL quiet on stderr.
// Errors are read back with CPLGetLastErrorMsg() and reported by us.
void gdalInit() {
    static std::once_flag once;
    std::call_once(once, [] {
        GDALAllRegister();
        CPLSetErrorHandler(CPLQuietErrorHandler);
    });
}

CrsInfo crsInfo(const OGRSpatialReference* srs) {
    CrsInfo c;
    if (srs == nullptr) return c;
    c.known = true;
    if (const char* n = srs->GetName()) c.name = n;

    // Declared EPSG code, or a guess from the definition (common for .prj files).
    const char* auth = srs->GetAuthorityName(nullptr);
    const char* code = srs->GetAuthorityCode(nullptr);
    if (auth && code && std::string(auth) == "EPSG") {
        c.epsg = std::atoi(code);
    } else {
        OGRSpatialReference copy(*srs);
        if (copy.AutoIdentifyEPSG() == OGRERR_NONE) {
            const char* guessed = copy.GetAuthorityCode(nullptr);
            if (guessed) c.epsg = std::atoi(guessed);
        }
    }
    return c;
}

} // namespace r2d
