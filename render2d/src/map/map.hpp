// SPDX-License-Identifier: AGPL-3.0-or-later
// A map: layers kept open, styles read once, drawn on any canvas for any view.
// What render2d's main() did, as a library: render2d.exe and editor2d both
// use it. See notions §45 to §47.
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "backend/gdal/gdal_layer.hpp"
#include "canvas/canvas.hpp"
#include "core/envelope.hpp"
#include "core/view.hpp"
#include "render/query_layer.hpp"
#include "render/render_layer.hpp"
#include "style/style.hpp"

namespace r2d {

struct LayerSpec {
    LayerSpec() = default;
    // {source, style}: the command line's form, the rest by default.
    LayerSpec(std::string src, std::optional<std::string> sty = std::nullopt)
        : source(std::move(src)), style(std::move(sty)) {}

    std::string                source;   // "file.fgb", or "file.gpkg::layer"
    std::optional<std::string> style;    // YSLD file; empty = default style
    bool                       visible = true;   // from a config file (§57)
    std::string                title;            // a readable name; render2d does not use it
    int                        colour = -1;      // default style's colour; -1 = its position (§58)
    std::string                crs;              // §70: its system, declared here ("EPSG:31300"), for a file
                                                 // that says none or a wrong one; "" = the file's
};

struct LayerReport {
    std::string      name;
    LayerRenderStats stats;              // renderLayer's numbers, unchanged
    std::string      reprojection;       // §70: "EPSG:4326 -> EPSG:31370 by ...", or "" (none needed)
};

// What one render did. reportLines() turns it into text. §47.
struct MapReport {
    CrsInfo                  crs;
    View                     view;               // pixelRatio included
    std::vector<LayerReport> layers;             // the visible layers drawn, in order
    bool                     hasLabels   = false; // labels were collected and drawn
    std::size_t              labelsDrawn = 0;
    double                   labelsMs    = 0.0;
    bool                     stopped     = false; // `stop` said true: layers skipped, no labels
    std::vector<std::string> warnings;            // found during this render
};

// One feature touched by Map::queryAt (notions §53).
struct QueryHit {
    int        layer = 0;        // index in the map, in drawing order
    FeatureId  fid = 0;          // §50
    Geometry   geometry;         // a copy: editor2d draws the selection with it
    Attributes attributes;       // every field, not only those of the style
};

// The features of a zone (§54, §56).
struct AreaQuery {
    std::vector<QueryHit> hits;              // the top layer first, as queryAt
    bool                  truncated = false;  // maxHits reached: the rest was not copied
    std::size_t           matched = 0;        // queryWhere: how many features matched in all (§64)
    std::string           error;              // the zone was refused (fewer than 3 points)
};

// "EPSG:31370 (BD72 / Belgian Lambert 72)", the name alone, or "none".
std::string crsLabel(const CrsInfo& crs);

// The lines render2d prints, from "map: ..." to "labels ... drawn".
std::vector<std::string> reportLines(const MapReport& r);

// Not thread-safe: use a Map from one thread only (GDAL layers). §46.
class Map {
public:
    // Never fails: a layer or a style that cannot be read becomes a warning.
    // With no layer at all, empty() is true and warnings() says why.
    // `crs`: the map's coordinate system (§70, crsFromUser); without it, the
    // first layer's (EPSG:3857 if that one is in degrees). Every layer in
    // another system is converted to it as it is read.
    static Map open(const std::vector<LayerSpec>& specs, double pixelRatio = 1.0,
                    const std::optional<CrsInfo>& crs = std::nullopt);

    // A map that changes (§58). addLayer opens the layer now and puts it at
    // `at` (0 = bottom; -1 or beyond the end = on top). Its warnings (missing
    // file, bad style, no CRS) go to `warnings`; false if it could not be
    // opened, and the map is unchanged. The first layer with a CRS gives the
    // map its own, unless one was given (§70).
    bool addLayer(const LayerSpec& spec, int at, std::vector<std::string>& warnings);
    void removeLayer(int i);
    void moveLayer(int from, int to);   // the layer at `from` ends at `to`

    // §59: a new style for an open layer, as YSLD text (image paths absolute,
    // or relative to the current folder). The source stays open. Unreadable
    // text: false, the old style stays, the reason in `warnings`.
    bool setLayerStyle(int i, const std::string& ysld, std::vector<std::string>& warnings);

    Map(Map&&) noexcept;
    Map& operator=(Map&&) noexcept;
    ~Map();

    bool                            empty() const { return layers_.empty(); }
    const std::vector<std::string>& warnings() const { return warnings_; }   // found while opening
    const CrsInfo&                  crs() const { return crs_; }              // §70: given, or the first declared one
    std::optional<Envelope>         extent() const;                           // all layers, visible or not, in the map's CRS

    // §70: another system for the map; every layer is converted to it from
    // now on. What could not be (no known way) goes to `warnings`.
    void setCrs(const CrsInfo& crs, std::vector<std::string>& warnings);

    int              layerCount() const { return static_cast<int>(layers_.size()); }
    const LayerInfo& layerInfo(int i) const;        // as the source says it: extent in ITS CRS
    std::optional<Envelope> layerExtent(int i) const;   // §70: the extent in the map's CRS (Zoom to layer)
    const std::string& layerReprojection(int i) const;  // §70: "EPSG:4326 -> ... by ...", or ""
    const CrsInfo&     layerCrs(int i) const;           // §70: the declared one (LayerSpec::crs), else the file's
    const std::vector<FieldDef>& layerFields(int i) const;   // the columns of the query's attributes
    bool             visible(int i) const;
    void             setVisible(int i, bool on);

    double pixelRatio() const { return pixelRatio_; }
    void   setPixelRatio(double r);    // re-scales from the original styles. §46

    // Every visible layer, then all their labels. `stop` is asked before each
    // layer; when it returns true, the rest is skipped (report.stopped).
    // The view's pixelRatio is replaced by the map's, so that the scale
    // and the styles always agree.
    MapReport render(Canvas& cv, const View& view, const std::function<bool()>& stop = {});

    // "What is here?" (§50 to §53): the features of the visible layers that
    // `pixel` (image pixels of `view`) touches, among those drawn at this
    // scale, with a tolerance in image pixels. The top layer first; in a
    // layer, the feature drawn last first.
    std::vector<QueryHit> queryAt(const View& view, Point pixel, double tolerancePx = 4.0);

    // "What is in this zone?" (§54): the features of the visible layers drawn
    // at this scale whose GEOMETRY shares a point with `ring` (map units,
    // closed or not; even-odd, so a bow-tie works). `view` gives the scale.
    // At most `maxHits` features in all (0 = no limit).
    AreaQuery queryArea(const View& view, const std::vector<Point>& ring, std::size_t maxHits = 0);

    // §64: every feature of layer `layer` whose attributes make `where` true
    // (the language of the styles' filters), whatever the scale and the
    // visibility. Two reads: the test with the fields of `where` only, then the
    // geometry and every field of the features kept. At most maxHits (0 = no
    // limit); beyond, truncated, and `matched` says how many there were.
    AreaQuery queryWhere(int layer, const std::string& where, std::size_t maxHits = 0);

private:
    struct Layer;
    Map();

    // One way to open a layer, for open() and addLayer() (§58). nullptr if
    // the source cannot be opened (the reason in `warnings`).
    std::unique_ptr<Layer> openLayer(const LayerSpec& spec, int colour, std::vector<std::string>& warnings) const;
    // §70: the layer seen in the map's CRS: a conversion when its own
    // differs, nothing when it is the same. What cannot be done: `warnings`.
    void bindLayer(Layer& layer, std::vector<std::string>& warnings) const;
    // §70: the map's CRS from a layer when none was given; true if it was set.
    bool takeCrsFrom(const Layer& layer, std::vector<std::string>& warnings);

    std::vector<std::unique_ptr<Layer>> layers_;
    std::vector<std::string>            warnings_;
    double                              pixelRatio_ = 1.0;
    CrsInfo                             crs_;               // §70: the map's system
    bool                                crsGiven_ = false;  // by --crs / crs: (else from the first layer)
};

} // namespace r2d
