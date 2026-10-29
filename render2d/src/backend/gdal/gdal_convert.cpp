// SPDX-License-Identifier: AGPL-3.0-or-later
#include "backend/gdal/gdal_convert.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

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
namespace {

// The folder of the running executable, or "" when unknown.
std::filesystem::path executableFolder() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return std::filesystem::path(buf).parent_path();
#else
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : p.parent_path();
#endif
}

// A path as UTF-8 text, the form GDAL and PROJ expect on every system.
std::string utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string g_gdalData = "built-in default";   // for gdalDataInfo()
std::string g_projData = "built-in default";
std::string g_projDir;                         // the folder alone, when chosen here (§70: PROJ's own context)

// notions §68: a render2d unzipped anywhere finds its reference files in
// share/gdal and share/proj beside the executable. An environment variable
// set by the user wins: nothing is changed then.
void findDataBesideExecutable() {
    const std::filesystem::path exe = executableFolder();
    std::error_code ec;

    if (const char* env = std::getenv("GDAL_DATA"); env && *env) {
        g_gdalData = std::string(env) + " (GDAL_DATA)";
    } else if (!exe.empty() && std::filesystem::is_directory(exe / "share" / "gdal", ec)) {
        const std::string dir = utf8(exe / "share" / "gdal");
        CPLSetConfigOption("GDAL_DATA", dir.c_str());
        g_gdalData = dir + " (beside the executable)";
    }

    const char* proj = std::getenv("PROJ_DATA");
    if (!proj || !*proj) proj = std::getenv("PROJ_LIB");
    if (proj && *proj) {
        g_projData = std::string(proj) + " (PROJ_DATA)";
    } else if (!exe.empty() && std::filesystem::exists(exe / "share" / "proj" / "proj.db", ec)) {
        const std::string dir = utf8(exe / "share" / "proj");
        const char* paths[] = {dir.c_str(), nullptr};
        OSRSetPROJSearchPaths(paths);
        g_projData = dir + " (beside the executable)";
        g_projDir  = dir;
    }
}

} // namespace

void gdalInit() {
    static std::once_flag once;
    std::call_once(once, [] {
        findDataBesideExecutable();
        GDALAllRegister();
        CPLSetErrorHandler(CPLQuietErrorHandler);
    });
}

const std::string& projDataFolder() {
    gdalInit();
    return g_projDir;
}

std::vector<std::string> gdalDataInfo() {
    gdalInit();
    return {"GDAL data: " + g_gdalData, "PROJ data: " + g_projData};
}

CrsInfo crsInfo(const OGRSpatialReference* srs) {
    CrsInfo c;
    if (srs == nullptr) return c;
    c.known = true;
    if (const char* n = srs->GetName()) c.name = n;
    c.geographic = srs->IsGeographic();
    // §70: the full definition, so that the map can build a conversion
    // without GDAL headers. WKT2: the most complete form.
    char* wkt = nullptr;
    const char* options[] = {"FORMAT=WKT2_2019", nullptr};
    if (srs->exportToWkt(&wkt, options) == OGRERR_NONE && wkt) c.wkt = wkt;
    CPLFree(wkt);

    // Declared EPSG code, or a guess from the definition (common for .prj files).
    const char* auth = srs->GetAuthorityName(nullptr);
    const char* code = srs->GetAuthorityCode(nullptr);
    if (auth && code) c.code = std::string(auth) + ":" + code;
    if (auth && code && std::string(auth) == "EPSG") {
        c.epsg = std::atoi(code);
    } else {
        OGRSpatialReference copy(*srs);
        if (copy.AutoIdentifyEPSG() == OGRERR_NONE) {
            const char* guessed = copy.GetAuthorityCode(nullptr);
            if (guessed) {
                c.epsg = std::atoi(guessed);
                c.code = std::string("EPSG:") + guessed;
            }
        }
    }
    return c;
}

} // namespace r2d
