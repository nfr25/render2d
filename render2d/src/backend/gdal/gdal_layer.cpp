// SPDX-License-Identifier: AGPL-3.0-or-later
#include "backend/gdal/gdal_layer.hpp"

#include <map>
#include <unordered_set>

#include <cstdlib>
#include <mutex>

#include <cpl_error.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include "backend/gdal/gdal_convert.hpp"

namespace r2d {

namespace {

std::optional<GeomType> layerType(OGRwkbGeometryType t) {
    switch (wkbFlatten(t)) {
        case wkbPoint:      case wkbMultiPoint:        return GeomType::Point;
        case wkbLineString: case wkbMultiLineString:
        case wkbCircularString: case wkbCompoundCurve:
        case wkbMultiCurve:                            return GeomType::Line;
        case wkbPolygon:    case wkbMultiPolygon:
        case wkbCurvePolygon: case wkbMultiSurface:    return GeomType::Polygon;
        default:                                       return std::nullopt;
    }
}

FieldType fieldType(OGRFieldType t) {
    switch (t) {
        case OFTInteger: case OFTInteger64: return FieldType::Integer;
        case OFTReal:                       return FieldType::Real;
        default:                            return FieldType::Text;   // dates compare as text, §27
    }
}

} // namespace

std::string gdalVersion() { return GDALVersionInfo("RELEASE_NAME"); }

bool zoneFromWkt(const std::string& wkt, std::vector<Point>& ring, std::string& error) {
    ring.clear();
    OGRGeometry* raw = nullptr;
    if (OGRGeometryFactory::createFromWkt(wkt.c_str(), nullptr, &raw) != OGRERR_NONE || !raw) {
        error = "not a WKT geometry: " + wkt;
        return false;
    }
    std::unique_ptr<OGRGeometry> g(raw);
    if (wkbFlatten(g->getGeometryType()) != wkbPolygon) {
        error = "a zone must be a POLYGON, got " + std::string(g->getGeometryName());
        return false;
    }
    const OGRLinearRing* outer = g->toPolygon()->getExteriorRing();
    if (!outer) {
        error = "empty POLYGON";
        return false;
    }
    for (int i = 0; i < outer->getNumPoints(); ++i)
        ring.push_back({outer->getX(i), outer->getY(i)});
    return true;
}

namespace {

std::mutex                         g_passwordsMutex;
std::map<std::string, std::string> g_passwords;   // connection -> password, this session only

// One value of a libpq connection string: 'quoted' when needed, \' and \\ escaped.
std::string conninfoValue(const std::string& v) {
    if (!v.empty() && v.find_first_of(" \t'\\") == std::string::npos) return v;
    std::string out = "'";
    for (char ch : v) {
        if (ch == '\'' || ch == '\\') out += '\\';
        out += ch;
    }
    return out + "'";
}

} // namespace

void setSessionPassword(const std::string& connection, const std::string& password) {
    std::lock_guard<std::mutex> lock(g_passwordsMutex);
    if (password.empty()) g_passwords.erase(connection);
    else                  g_passwords[connection] = password;
}

std::string withSessionPassword(const std::string& source) {
    if (source.rfind("PG:", 0) != 0) return source;
    std::lock_guard<std::mutex> lock(g_passwordsMutex);
    const auto it = g_passwords.find(source);
    return it == g_passwords.end() ? source : source + " password=" + conninfoValue(it->second);
}

std::vector<LayerInfo> listLayers(const std::string& source, std::string& error) {
    gdalInit();
    CPLErrorReset();
    std::vector<LayerInfo> out;
    GDALDatasetUniquePtr ds(GDALDataset::Open(withSessionPassword(source).c_str(), GDAL_OF_VECTOR));
    if (!ds) {
        error = "cannot open '" + source + "'";
        if (CPLGetLastErrorMsg()[0] != '\0') error += std::string(": ") + CPLGetLastErrorMsg();
        return out;
    }
    for (int i = 0; i < ds->GetLayerCount(); ++i) {
        OGRLayer* layer = ds->GetLayer(i);
        LayerInfo info;
        info.source         = source;
        info.name           = layer->GetName();
        info.type           = layerType(layer->GetGeomType());
        info.featureCount   = static_cast<long long>(layer->GetFeatureCount(FALSE));   // -1: costly
        info.hasGeometry    = wkbFlatten(layer->GetGeomType()) != wkbNone;
        info.crs            = crsInfo(layer->GetSpatialRef());
        info.layersInSource = ds->GetLayerCount();
        out.push_back(std::move(info));
    }
    return out;
}

struct GdalLayer::Impl {
    GDALDatasetUniquePtr  dataset;
    OGRLayer*             layer = nullptr;   // owned by the dataset
    LayerInfo             info;
    std::vector<FieldDef> fields;
    std::vector<int>      needed;            // columns to read, see setNeededFields
};

GdalLayer::GdalLayer() : impl_(std::make_unique<Impl>()) {}
GdalLayer::~GdalLayer() = default;

std::unique_ptr<GdalLayer> GdalLayer::open(const std::string& source,
                                           const std::string& layerName,
                                           std::string& error) {
    gdalInit();
    CPLErrorReset();

    std::unique_ptr<GdalLayer> l(new GdalLayer());
    l->impl_->dataset.reset(GDALDataset::Open(withSessionPassword(source).c_str(), GDAL_OF_VECTOR));
    if (!l->impl_->dataset) {
        error = "cannot open '" + source + "'";
        if (CPLGetLastErrorMsg()[0] != '\0') error += std::string(": ") + CPLGetLastErrorMsg();
        return nullptr;
    }

    GDALDataset& ds = *l->impl_->dataset;
    OGRLayer* layer = layerName.empty() ? (ds.GetLayerCount() > 0 ? ds.GetLayer(0) : nullptr)
                                        : ds.GetLayerByName(layerName.c_str());
    if (layer == nullptr) {
        error = layerName.empty() ? "'" + source + "' has no vector layer"
                                  : "layer '" + layerName + "' not found in '" + source + "'";
        return nullptr;
    }
    l->impl_->layer = layer;

    LayerInfo& info = l->impl_->info;
    info.source       = source;
    info.name         = layer->GetName();
    info.type         = layerType(layer->GetGeomType());
    info.featureCount = static_cast<long long>(layer->GetFeatureCount(FALSE));
    info.hasGeometry  = wkbFlatten(layer->GetGeomType()) != wkbNone;
    info.crs          = crsInfo(layer->GetSpatialRef());
    info.layersInSource = ds.GetLayerCount();

    OGREnvelope env;
    if (layer->GetExtent(&env, TRUE) == OGRERR_NONE) {
        info.extent = Envelope{env.MinX, env.MinY, env.MaxX, env.MaxY};
    }

    const OGRFeatureDefn* defn = layer->GetLayerDefn();
    for (int i = 0; i < defn->GetFieldCount(); ++i) {
        const OGRFieldDefn* f = defn->GetFieldDefn(i);
        l->impl_->fields.push_back({f->GetNameRef(), fieldType(f->GetType())});
    }
    l->setNeededFields({});   // geometries only, until a filter asks for more
    return l;
}

const LayerInfo& GdalLayer::info() const { return impl_->info; }

const std::vector<FieldDef>& GdalLayer::fields() const { return impl_->fields; }

void GdalLayer::setNeededFields(const std::vector<int>& columns) {
    impl_->needed.clear();
    std::vector<bool> keep(impl_->fields.size(), false);
    for (int c : columns) {
        if (c >= 0 && c < static_cast<int>(keep.size()) && !keep[c]) { keep[c] = true; impl_->needed.push_back(c); }
    }
    // A NULL-terminated list of the names GDAL may skip, plus the style string.
    std::vector<const char*> ignored;
    for (std::size_t i = 0; i < keep.size(); ++i)
        if (!keep[i]) ignored.push_back(impl_->fields[i].name.c_str());
    ignored.push_back("OGR_STYLE");
    ignored.push_back(nullptr);
    impl_->layer->SetIgnoredFields(ignored.data());
}

void GdalLayer::setSpatialFilter(const Envelope& e) {
    impl_->layer->SetSpatialFilterRect(e.xmin, e.ymin, e.xmax, e.ymax);
}

namespace {

// One feature: its needed fields into `attrs`, its geometry into `geom`, then
// `visit` (§17, §27). Shared by forEach and forFids.
void visitFeature(OGRFeature& feature, const std::vector<int>& needed, const std::vector<FieldDef>& fields,
                  Geometry& geom, Attributes& attrs, ReadStats& stats, const FeatureVisitor& visit) {
    ++stats.read;
    for (int c : needed) {
        if (!feature.IsFieldSetAndNotNull(c)) { attrs[c] = std::monostate{}; continue; }
        switch (fields[c].type) {
            case FieldType::Integer: attrs[c] = static_cast<long long>(feature.GetFieldAsInteger64(c)); break;
            case FieldType::Real:    attrs[c] = feature.GetFieldAsDouble(c);                            break;
            case FieldType::Text:    // reuse the string's memory from the previous feature
                if (auto* t = std::get_if<std::string>(&attrs[c])) t->assign(feature.GetFieldAsString(c));
                else attrs[c] = std::string(feature.GetFieldAsString(c));
                break;
        }
    }
    switch (toGeometry(feature.GetGeometryRef(), geom)) {
        case ConvertResult::Ok:          ++stats.drawn; visit(geom, attrs, feature.GetFID()); break;
        case ConvertResult::Empty:       ++stats.empty;                     break;
        case ConvertResult::Unsupported: ++stats.unsupported;               break;
    }
}

} // namespace

ReadStats GdalLayer::forEach(const FeatureVisitor& visit) {
    ReadStats  stats;
    Geometry   geom;                                     // reused: keeps its memory
    Attributes attrs(impl_->fields.size());              // reused too; unread columns stay NULL
    OGRLayer*  layer = impl_->layer;
    layer->ResetReading();
    for (auto& feature : layer)   // OGRFeatureUniquePtr, freed automatically
        visitFeature(*feature, impl_->needed, impl_->fields, geom, attrs, stats, visit);
    return stats;
}

void GdalLayer::clearSpatialFilter() {
    impl_->layer->SetSpatialFilter(nullptr);
}

ReadStats GdalLayer::forFids(const std::vector<FeatureId>& fids, const FeatureVisitor& visit) {
    ReadStats  stats;
    Geometry   geom;
    Attributes attrs(impl_->fields.size());
    OGRLayer*  layer = impl_->layer;
    if (layer->TestCapability(OLCRandomRead)) {
        // Shapefile, GeoPackage, FlatGeobuf...: straight to each feature.
        for (FeatureId fid : fids) {
            OGRFeatureUniquePtr feature(layer->GetFeature(fid));
            if (feature) visitFeature(*feature, impl_->needed, impl_->fields, geom, attrs, stats, visit);
        }
        return stats;
    }
    // No random access (a GeoJSON...): one read, keeping the wanted ones.
    std::unordered_set<FeatureId> wanted(fids.begin(), fids.end());
    layer->ResetReading();
    for (auto& feature : layer)
        if (wanted.count(feature->GetFID()))
            visitFeature(*feature, impl_->needed, impl_->fields, geom, attrs, stats, visit);
    return stats;
}

} // namespace r2d
