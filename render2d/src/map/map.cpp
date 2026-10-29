// SPDX-License-Identifier: AGPL-3.0-or-later
#include "map/map.hpp"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "backend/gdal/gdal_raster.hpp"
#include "backend/gdal/gdal_reproject.hpp"
#include "render/labels.hpp"
#include "render/symbolize.hpp"
#include "style/expression.hpp"
#include "style/ysld_reader.hpp"

namespace r2d {

namespace {

// printf into a std::string: the report keeps render2d's exact formats.
std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char small[512];
    const int n = std::vsnprintf(small, sizeof small, fmt, args);
    va_end(args);
    if (n < static_cast<int>(sizeof small)) return std::string(small, n > 0 ? n : 0);

    std::string big(static_cast<std::size_t>(n) + 1, '\0');
    va_start(args, fmt);
    std::vsnprintf(big.data(), big.size(), fmt, args);
    va_end(args);
    big.resize(static_cast<std::size_t>(n));
    return big;
}

constexpr GeomType kTypes[3] = {GeomType::Point, GeomType::Line, GeomType::Polygon};

// Above this size, a raster without overviews is slow to show whole (§65).
constexpr int kOverviewsWanted = 4096;

// A raster layer at this view (§66): the window read once, then laid once
// per raster symbolizer of the active rules (no style: once, opaque).
// Filters mean nothing here (no attributes): they are not looked at.
LayerRenderStats renderRaster(GdalRaster& raster, Canvas& cv, const View& v, const Style* style,
                              std::vector<std::string>& warnings) {
    using Clock = std::chrono::steady_clock;
    LayerRenderStats st;
    st.raster = true;
    std::vector<double> opacities;
    if (!style) {
        st.passes = 1;
        opacities.push_back(1.0);
    } else {
        for (const FeatureTypeStyle& fts : style->featureStyles) {
            const std::vector<const Rule*> rules = activeRules(fts, v.scaleDenominator());
            if (!rules.empty()) ++st.passes;
            for (const Rule* rule : rules)
                for (const Symbolizer& sym : rule->symbolizers)
                    if (const auto* r = std::get_if<RasterSymbolizer>(&sym)) opacities.push_back(r->opacity);
        }
        if (st.passes > 0 && opacities.empty())
            warnings.push_back(raster.info().name + ": the style has no 'raster' symbolizer at this scale: nothing drawn");
    }
    if (opacities.empty()) return st;

    std::string error;
    const RasterRead rr = raster.read(v, error);
    if (!error.empty()) warnings.push_back(error);
    st.readMs       = rr.ms;
    st.pixelsWidth  = rr.block.width;
    st.pixelsHeight = rr.block.height;
    st.fileWidth    = rr.fileWidth;
    st.fileHeight   = rr.fileHeight;
    st.reduced      = rr.reduced;
    if (rr.block.argb.empty()) return st;

    const auto t0 = Clock::now();
    for (double opacity : opacities) cv.drawPixels(rr.block, rr.x, rr.y, opacity);
    st.drawMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return st;
}

} // namespace

// ---- one layer: what lives as long as the map (§46) --------------------------

struct Map::Layer {
    std::unique_ptr<GdalLayer> source;           // a vector layer...
    std::unique_ptr<GdalRaster> raster;          // ...or a raster (§65): one of the two
    int                        index = 0;        // the default style colour: its position when opened (§46), kept when it moves (§58)
    bool                       visible = true;

    std::optional<Style>       style;            // from --style, as read (original)
    std::optional<Style>       scaledStyle;      // the same, times the pixel ratio
    std::array<Style, 3>       defaults;         // per GeomType, scaled

    // §70: in the map's CRS. Both empty when the layer's CRS is the map's.
    std::unique_ptr<CrsTransform>     toMap;
    std::unique_ptr<ReprojectedLayer> reprojected;
    std::optional<Envelope>           mapExtent;      // the extent, converted
    bool                              unusable = false;   // another CRS, and no way to the map's: not drawn

    // What renderLayer and the queries read: the layer, or the layer converted.
    FeatureSource& features() { return reprojected ? static_cast<FeatureSource&>(*reprojected) : *source; }

    void scale(double ratio) {
        if (style) scaledStyle = r2d::scaledStyle(*style, ratio);
        for (int t = 0; t < 3; ++t) defaults[t] = r2d::scaledStyle(defaultStyle(kTypes[t], index), ratio);
    }

    const LayerInfo& info() const { return source ? source->info() : raster->info(); }

    StyleByType styles() const {
        if (scaledStyle) return {&*scaledStyle, &*scaledStyle, &*scaledStyle};
        return {&defaults[0], &defaults[1], &defaults[2]};
    }
};

Map::Map() = default;
Map::Map(Map&&) noexcept = default;
Map& Map::operator=(Map&&) noexcept = default;
Map::~Map() = default;

// ---- opening: once (§46) --------------------------------------------------------

std::unique_ptr<Map::Layer> Map::openLayer(const LayerSpec& spec, int colour,
                                           std::vector<std::string>& warnings) const {
    std::string source = spec.source, layerName;
    if (const auto sep = spec.source.rfind("::"); sep != std::string::npos) {
        source    = spec.source.substr(0, sep);
        layerName = spec.source.substr(sep + 2);
    }
    // A vector layer, or else a raster (§65). Neither: the message that
    // says the most (a TIFF without position is a raster with a problem).
    std::string error;
    auto gdal = GdalLayer::open(source, layerName, error);
    std::unique_ptr<GdalRaster> raster;
    if (!gdal && layerName.empty()) {
        std::string rasterError;
        raster = GdalRaster::open(source, rasterError);
        if (!raster && rasterError.rfind("cannot open", 0) != 0) error = rasterError;
    }
    if (!gdal && !raster) { warnings.push_back(error); return nullptr; }

    auto layer     = std::make_unique<Layer>();
    layer->source  = std::move(gdal);
    layer->raster  = std::move(raster);
    if (layer->raster && !layer->raster->hasOverviews() &&
        std::max(layer->raster->width(), layer->raster->height()) > kOverviewsWanted)
        warnings.push_back(layer->info().name + ": " + std::to_string(layer->raster->width()) + "x" +
                           std::to_string(layer->raster->height()) + " pixels and no overviews: a view of the whole "
                           "reads every pixel. Once for all: gdaladdo -r average \"" + source + "\" 2 4 8 16 32 64");
    layer->index   = colour;   // §58: the default style's colour, kept when the layer moves
    layer->visible = spec.visible;

    // Style file: warnings are reported, a bad file falls back to the default.
    if (spec.style) {
        YsldResult r = readYsldFile(*spec.style);
        for (const auto& w : r.warnings) warnings.push_back(*spec.style + ": " + w);
        if (r.style) layer->style = std::move(*r.style);
        else         warnings.push_back(*spec.style + ": " + r.error + " (default style used)");
    }
    layer->scale(pixelRatio_);   // §39
    return layer;
}

// §70: a hand-made GeoJSON in Lambert metres claims degrees (crsBelievable).
static bool notReallyDegrees(const LayerInfo& li) { return !crsBelievable(li.crs, li.extent); }

// §70: the layer in the map's CRS. The same system: nothing at all (the
// image is the same to the byte). Another: a conversion, prepared once.
void Map::bindLayer(Layer& layer, std::vector<std::string>& warnings) const {
    layer.reprojected.reset();
    layer.toMap.reset();
    layer.mapExtent.reset();
    layer.unusable = false;
    const LayerInfo& li = layer.info();
    if (!li.crs.known) {
        if (crs_.known) warnings.push_back(li.name + ": no CRS declared, assumed to be the map CRS");
        return;
    }
    if (notReallyDegrees(li)) {
        if (crs_.known)   // as for "no CRS declared": said only when there is a map CRS to assume
            warnings.push_back(li.name + ": declared in degrees (" + crsLabel(li.crs) + ") but its coordinates are not "
                               "(GeoJSON without \"crs\"?): assumed to be the map CRS");
        return;
    }
    if (sameCrs(li.crs, crs_)) return;
    if (layer.raster) {   // 4b-2: warping
        warnings.push_back(li.name + ": CRS " + crsLabel(li.crs) + " differs from the map CRS " + crsLabel(crs_) +
                           ": a raster is not reprojected yet (step 4b-2), not drawn");
        layer.unusable = true;
        return;
    }
    std::string error;
    layer.toMap = CrsTransform::create(li.crs, crs_, li.extent, error);
    if (!layer.toMap) {
        warnings.push_back(li.name + ": " + error + ", not drawn");
        layer.unusable = true;
        return;
    }
    layer.reprojected = std::make_unique<ReprojectedLayer>(*layer.source, *layer.toMap);
    if (li.extent) layer.mapExtent = layer.toMap->forwardBounds(*li.extent);
}

bool Map::takeCrsFrom(const Layer& layer, std::vector<std::string>& warnings) {
    if (crsGiven_ || crs_.known) return false;
    const CrsInfo& c = layer.info().crs;
    if (!c.known || notReallyDegrees(layer.info())) return false;
    crs_ = crsForMap(c);
    if (c.geographic)   // §70: a map in degrees is refused
        warnings.push_back(layer.info().name + " is in degrees (" + crsLabel(c) +
                           "): the map uses EPSG:3857 (Web Mercator); --crs chooses another");
    return true;
}

void Map::setCrs(const CrsInfo& crs, std::vector<std::string>& warnings) {
    crs_      = crs;
    crsGiven_ = true;
    for (auto& layer : layers_) bindLayer(*layer, warnings);
}

Map Map::open(const std::vector<LayerSpec>& specs, double pixelRatio, const std::optional<CrsInfo>& crs) {
    Map map;
    map.pixelRatio_ = pixelRatio;
    if (crs) {
        map.crs_      = *crs;
        map.crsGiven_ = true;
    }

    // 1. Open the layers. A missing one is a warning: the others are drawn.
    //    The colour: given, or the position among the layers OPENED (§46).
    for (const LayerSpec& spec : specs) {
        const int colour = spec.colour >= 0 ? spec.colour : static_cast<int>(map.layers_.size());
        if (auto layer = map.openLayer(spec, colour, map.warnings_)) map.layers_.push_back(std::move(layer));
    }
    // 2. The map's CRS (§70): given, or the first declared one; then each
    //    layer in it.
    for (const auto& layer : map.layers_)
        if (map.takeCrsFrom(*layer, map.warnings_)) break;
    for (const auto& layer : map.layers_) map.bindLayer(*layer, map.warnings_);
    return map;
}

bool Map::addLayer(const LayerSpec& spec, int at, std::vector<std::string>& warnings) {
    const int colour = spec.colour >= 0 ? spec.colour : layerCount();
    auto layer = openLayer(spec, colour, warnings);
    if (!layer) return false;
    if (at < 0 || at > layerCount()) at = layerCount();   // on top
    Layer& added = *layer;
    layers_.insert(layers_.begin() + at, std::move(layer));
    if (takeCrsFrom(added, warnings)) {
        for (auto& l : layers_) bindLayer(*l, warnings);   // the first with a CRS: the others follow it
    } else {
        bindLayer(added, warnings);
    }
    return true;
}

void Map::removeLayer(int i) {
    if (i < 0 || i >= layerCount()) return;
    layers_.erase(layers_.begin() + i);
}

bool Map::setLayerStyle(int i, const std::string& ysld, std::vector<std::string>& warnings) {
    if (i < 0 || i >= layerCount()) return false;
    Layer& layer = *layers_[static_cast<std::size_t>(i)];
    YsldResult r = parseYsld(ysld);
    for (const auto& w : r.warnings) warnings.push_back(layer.info().name + " style: " + w);
    if (!r.style) {
        warnings.push_back(layer.info().name + " style: " + r.error + " (previous style kept)");
        return false;
    }
    layer.style = std::move(*r.style);
    layer.scale(pixelRatio_);   // §39
    return true;
}

void Map::moveLayer(int from, int to) {
    const int n = layerCount();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    auto layer = std::move(layers_[static_cast<std::size_t>(from)]);
    layers_.erase(layers_.begin() + from);
    layers_.insert(layers_.begin() + to, std::move(layer));
}

std::optional<Envelope> Map::extent() const {
    std::optional<Envelope> area;
    for (int i = 0; i < layerCount(); ++i)
        if (const auto e = layerExtent(i)) area = area ? area->merged(*e) : *e;
    return area;
}

std::optional<Envelope> Map::layerExtent(int i) const {
    const Layer& layer = *layers_.at(static_cast<std::size_t>(i));
    if (layer.unusable) return std::nullopt;
    return layer.toMap ? layer.mapExtent : layer.info().extent;
}

const std::string& Map::layerReprojection(int i) const {
    static const std::string none;
    const Layer& layer = *layers_.at(static_cast<std::size_t>(i));
    return layer.toMap ? layer.toMap->description() : none;
}

const LayerInfo& Map::layerInfo(int i) const { return layers_.at(i)->info(); }
const std::vector<FieldDef>& Map::layerFields(int i) const {
    static const std::vector<FieldDef> none;   // a raster has no field
    const Layer& layer = *layers_.at(i);
    return layer.source ? layer.source->fields() : none;
}
bool Map::visible(int i) const { return layers_.at(i)->visible; }
void Map::setVisible(int i, bool on) { layers_.at(i)->visible = on; }

void Map::setPixelRatio(double r) {
    if (r == pixelRatio_) return;
    pixelRatio_ = r;
    for (auto& layer : layers_) layer->scale(r);   // always from the originals
}

// ---- rendering: at each view ------------------------------------------------------

MapReport Map::render(Canvas& cv, const View& view, const std::function<bool()>& stop) {
    using Clock = std::chrono::steady_clock;

    MapReport report;
    report.crs  = crs_;
    report.view = view;
    report.view.pixelRatio = pixelRatio_;   // §39: scale and styles agree
    const View& v = report.view;
    auto warn = [&](const std::string& w) { report.warnings.push_back(w); };

    std::vector<Label> labels;   // §35: drawn after all the layers, above them
    for (const auto& layer : layers_) {
        if (!layer->visible) continue;
        if (stop && stop()) { report.stopped = true; break; }

        const LayerInfo& li = layer->info();
        if (layer->unusable) {   // §70: said when opened
            warn(li.name + ": not drawn, its CRS cannot be converted to the map's");
            continue;
        }
        if (layer->raster) {   // §66
            const LayerRenderStats st = renderRaster(*layer->raster, cv, v,
                                                     layer->scaledStyle ? &*layer->scaledStyle : nullptr,
                                                     report.warnings);
            report.layers.push_back({li.name, st, {}});
            if (st.passes == 0) warn(li.name + ": no rule of the style applies at scale 1:" +
                                     std::to_string(static_cast<long>(std::lround(v.scaleDenominator()))));
            continue;
        }
        const LayerRenderStats st = renderLayer(layer->features(), cv, v, layer->styles(), &labels);
        report.layers.push_back({li.name, st, layer->toMap ? layer->toMap->description() : std::string()});

        const ReadStats& rs = st.read;
        if (!st.missingFields.empty()) {
            std::string all;
            for (const FieldDef& f : layer->source->fields()) all += (all.empty() ? "" : ", ") + f.name;
            for (const auto& m : st.missingFields)
                warn(li.name + ": field '" + m + "' named in a filter does not exist, it counts as NULL (fields: " + all + ")");
        }
        for (const auto& e : st.imageErrors) warn(li.name + ": " + e + " (the mark is drawn instead)");
        if (st.notNumber)
            warn(li.name + ": " + std::to_string(st.notNumber) + " values compared with a number are not numbers "
                 "(e.g. '75B' < 100): the comparison is unknown, the rule does not apply");
        if (st.passes == 0) warn(li.name + ": no rule of the style applies at scale 1:" +
                                 std::to_string(static_cast<long>(std::lround(v.scaleDenominator()))));
        if (rs.empty)       warn(li.name + ": " + std::to_string(rs.empty) + " empty geometries skipped");
        if (rs.unsupported) warn(li.name + ": " + std::to_string(rs.unsupported) + " geometries of an unsupported type skipped");
        if (rs.outside)     warn(li.name + ": " + std::to_string(rs.outside) + " features outside the map's system skipped");
    }

    // The labels of all the layers, on top. §35. Not for an abandoned view.
    if (!labels.empty() && !report.stopped) {
        const auto l0 = Clock::now();
        report.labelsDrawn = drawLabels(cv, labels);
        report.labelsMs    = std::chrono::duration<double, std::milli>(Clock::now() - l0).count();
        report.hasLabels   = true;
    }
    return report;
}

// ---- querying (§50 to §53) ----------------------------------------------------------

std::vector<QueryHit> Map::queryAt(const View& view, Point pixel, double tolerancePx) {
    View v = view;
    v.pixelRatio = pixelRatio_;   // the scale of the styles, as for render (§39)
    const Point world = v.imageToWorld.apply(pixel);

    std::vector<QueryHit> hits;
    for (int i = layerCount() - 1; i >= 0; --i) {   // the top layer first (§52)
        Layer& layer = *layers_[static_cast<std::size_t>(i)];
        if (!layer.visible || !layer.source || layer.unusable) continue;   // a raster has no feature (§66)
        for (LayerHit& h : queryLayer(layer.features(), v, world, tolerancePx, layer.styles()))
            hits.push_back({i, h.fid, std::move(h.geometry), std::move(h.attributes)});
    }
    return hits;
}

AreaQuery Map::queryArea(const View& view, const std::vector<Point>& ring, std::size_t maxHits) {
    AreaQuery out;
    View v = view;
    v.pixelRatio = pixelRatio_;   // the scale of the styles, as for render (§39)

    // The zone: one ring, without the closing point and without repeated points.
    Geometry zone;
    zone.type = GeomType::Polygon;
    for (const Point& p : ring)
        if (zone.points.empty() || p.x != zone.points.back().x || p.y != zone.points.back().y)
            zone.points.push_back(p);
    if (zone.points.size() > 1 && zone.points.front().x == zone.points.back().x &&
        zone.points.front().y == zone.points.back().y)
        zone.points.pop_back();
    if (zone.points.size() < 3) {
        out.error = "a zone needs at least 3 distinct points";
        return out;
    }
    zone.closePart(0);

    for (int i = layerCount() - 1; i >= 0 && !out.truncated; --i) {   // the top layer first
        Layer& layer = *layers_[static_cast<std::size_t>(i)];
        if (!layer.visible || !layer.source || layer.unusable) continue;   // a raster has no feature (§66)
        // What is left for this layer; 0 left: one more feature makes it truncated.
        const std::size_t left = maxHits == 0 ? kNoLimit : maxHits - out.hits.size();
        for (LayerHit& h : queryLayerArea(layer.features(), v, zone, layer.styles(), left, out.truncated))
            out.hits.push_back({i, h.fid, std::move(h.geometry), std::move(h.attributes)});
    }
    return out;
}

AreaQuery Map::queryWhere(int i, const std::string& where, std::size_t maxHits) {
    AreaQuery out;
    if (i < 0 || i >= layerCount()) {
        out.error = "no layer " + std::to_string(i);
        return out;
    }
    std::string error;
    std::optional<Expression> e = Expression::parseCondition(where, error);
    if (!e) {
        out.error = error;
        return out;
    }
    if (!layers_[static_cast<std::size_t>(i)]->source) {
        out.error = layerInfo(i).name + " is a raster: it has no attributes";
        return out;
    }
    Layer& layer = *layers_[static_cast<std::size_t>(i)];
    if (layer.unusable) {
        out.error = layerInfo(i).name + ": its CRS cannot be converted to the map's";
        return out;
    }
    GdalLayer& source = *layer.source;
    if (const auto unknown = e->bind(source.fields()); !unknown.empty()) {
        out.error = "no field '" + unknown.front() + "' in " + source.info().name;
        return out;
    }

    // 1. The test: the whole layer, the fields of `where` only.
    source.clearSpatialFilter();
    source.setNeededFields(e->columns());
    std::vector<FeatureId> fids;
    EvalStats stats;
    source.forEach([&](const Geometry&, const Attributes& a, FeatureId fid) {
        if (e->test(a, stats) != Tri::True) return;
        ++out.matched;
        if (maxHits == 0 || fids.size() < maxHits) fids.push_back(fid);
    });
    out.truncated = maxHits != 0 && out.matched > maxHits;

    // 2. The features kept: their geometry and every field.
    std::vector<int> all(source.fields().size());
    for (std::size_t c = 0; c < all.size(); ++c) all[c] = static_cast<int>(c);
    source.setNeededFields(all);
    // §70: the geometries in the map's CRS, as for a click.
    auto keep = [&](const Geometry& g, const Attributes& a, FeatureId fid) { out.hits.push_back({i, fid, g, a}); };
    if (layer.reprojected) layer.reprojected->forFids(fids, keep);
    else                   source.forFids(fids, keep);
    return out;
}

// ---- the report as text (§47) ------------------------------------------------------

std::string crsLabel(const CrsInfo& c) {
    if (!c.known) return "none";
    if (c.epsg != 0) return "EPSG:" + std::to_string(c.epsg) + " (" + c.name + ")";
    if (!c.code.empty()) return c.code + " (" + c.name + ")";   // "OGC:CRS84 (WGS 84 (CRS84))"
    return c.name;
}

std::vector<std::string> reportLines(const MapReport& r) {
    std::vector<std::string> lines;
    const View& v = r.view;

    std::string head = format("map: %s, %dx%d px, %.3f m/px, scale 1:%.0f", crsLabel(r.crs).c_str(),
                              v.size.width, v.size.height, v.resolution, v.scaleDenominator());
    if (v.pixelRatio != 1.0)
        head += format(" (pixel ratio %g: %.3f m per pixel seen)", v.pixelRatio, v.resolution * v.pixelRatio);
    lines.push_back(head);

    for (const LayerReport& lr : r.layers) {
        const LayerRenderStats& st = lr.stats;
        if (st.raster) {   // §66: pixels, not features
            lines.push_back(format("  %-20s raster %dx%d px from %.0fx%.0f file px (%s) %9.1f ms  (read %.1f ms, draw %.1f ms)",
                                   lr.name.c_str(), st.pixelsWidth, st.pixelsHeight, st.fileWidth, st.fileHeight,
                                   st.reduced ? "averaged" : "nearest", st.readMs + st.drawMs, st.readMs, st.drawMs));
            continue;
        }
        std::string line = format("  %-20s %8zu read %8zu drawn %9.1f ms  (read %.1f ms, ", lr.name.c_str(),
                                  st.read.read, st.read.drawn, st.readMs + st.filterMs + st.drawMs, st.readMs);
        if (st.filtered) line += format("filters %.1f ms, ", st.filterMs);
        line += format("draw %.1f ms, %d pass%s)", st.drawMs, st.passes, st.passes == 1 ? "" : "es");
        lines.push_back(line);
        if (!lr.reprojection.empty()) lines.push_back("      reprojected " + lr.reprojection);   // §70

        // §30, §31: what the filters read and what each rule drew.
        if (st.filtered) {
            std::string fields;
            for (const auto& f : st.fieldsRead) fields += (fields.empty() ? "" : ", ") + f;
            lines.push_back(format("      fields read: %s", fields.empty() ? "none" : fields.c_str()));
            for (const RuleCount& rc : st.rules)
                lines.push_back(format("      rule %-24s %8zu drawn", rc.name.c_str(), rc.drawn));
            if (st.labels || st.labelsEmpty)
                lines.push_back(format("      labels %8zu (%zu without text)", st.labels, st.labelsEmpty));
        }
    }

    if (r.hasLabels) lines.push_back(format("  labels %zu drawn %.1f ms", r.labelsDrawn, r.labelsMs));
    if (r.stopped)   lines.push_back("  (stopped: a newer view was asked for)");
    return lines;
}

} // namespace r2d
