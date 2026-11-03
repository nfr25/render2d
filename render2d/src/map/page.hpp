// SPDX-License-Identifier: AGPL-3.0-or-later
// A printed page (notions §74): a sheet (A4, A3, portrait or landscape), the
// map in a frame at an exact scale on the ground, a title, a scale bar, a
// north arrow pointing to the true north, the attributions. Written as PDF
// (or SVG): the canvas unit is then the typographer's point, 0.3528 mm.
// render2d's command line and editor2d compose the same page with this.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/envelope.hpp"
#include "core/point.hpp"
#include "core/view.hpp"
#include "map/map.hpp"

namespace r2d {

class Canvas;

// One point (1/72 inch) in millimetres, and OGC's pixel (§39) in points.
inline constexpr double kPointMm = 25.4 / 72.0;               // 0.3528 mm
inline constexpr double kPixelPerPoint = 0.28 / (25.4 / 72.0);   // 0.7937: the map's pixel ratio on paper

// What the page should be. Read from --page and its options, or from the
// print: section of a map file (the command line has the last word, §57).
struct PageSpec {
    std::string             format = "A4";     // "A4", "A3"
    bool                    landscape = true;
    std::optional<double>   scale;             // 1000 for 1:1 000, on the ground; none: fit the area
    std::optional<Point>    center;            // in the map's system; none: the area's centre
    std::string             title;
    std::string             subtitle;
    std::string             date;              // none: today, 2026-11-03
    std::string             generator;         // "render2d 0.5.0": written in small letters
    bool                    scaleBar = true;
    bool                    northArrow = true;
    bool                    legend = true;      // §74 (5b): --no-legend, legend: false in print:
};

// "A4-landscape", "A3-portrait", "a4" (portrait by default as paper is): false
// and `error` for anything else.
bool parsePageFormat(const std::string& text, PageSpec& spec, std::string& error);
std::string pageFormatText(const PageSpec& spec);   // back to "A4-landscape"

// Where everything goes, in points from the sheet's top-left corner, and the
// view of the map in its frame. Planned before drawing, so that a caller
// (editor2d's dialog) can show the page's outline on the map.
struct PagePlan {
    double                   width = 0, height = 0;   // the sheet, in points
    double                   frameX = 0, frameY = 0, frameW = 0, frameH = 0;   // the map's frame
    View                     view;                    // the map in the frame, its size in whole points
    double                   scale = 0;               // the scale on the ground, at the centre (1000)
    double                   mapPerGround = 1.0;      // map units per metre of ground there (Mercator: 1.58)
    std::optional<double>    north;                   // §74: the true north, degrees from "up"
    std::vector<std::string> warnings;
};

// The plan for `map`: the scale asked for, or the one that fits `area` (else
// the map's extent), rounded to a nice value. Empty, with `error`, when there
// is nothing to place (no area, no system in metres...).
std::optional<PagePlan> planPage(const Map& map, const PageSpec& spec, const std::optional<Envelope>& area,
                                 std::string& error);

// Draws the page: a white sheet, the map in its frame (clipped, at the pixel
// ratio of paper), then the title, the scale bar, the north arrow, the small
// print. The map's pixel ratio is put back as it was. The report is the map's.
MapReport composePage(Map& map, const PageSpec& spec, const PagePlan& plan, Canvas& cv);

// §74, for editor2d's Print dialog (the page's outline drawn on the map):
// the map's frame on the sheet, in millimetres (A4 landscape: 277 x 150);
// the map's units per metre of ground at `p` (1 when the system is unknown);
// the next round scale (1, 2, 2.5, 5 x 10^n) at or above `scale`.
struct FrameMm { double width = 0, height = 0; };
FrameMm pageFrame(const PageSpec& spec);
double  mapPerGroundAt(const CrsInfo& crs, Point p);
double  niceScale(double scale);

// "1:1 000", "1:25 000": spaces between the thousands.
std::string scaleText(double scale);

// §74 (5b): the legend. A group per layer seen on the page (top layer
// first), a line per rule active at the page's scale; the rules of the
// style's passes matched by name, their symbolizers stacked in pass order
// (the OSM roads: the casing under the fill). Not the web basemaps nor the
// rasters, not the text symbolizers, not a layer with legend: false.
struct LegendLine {
    std::string             label;      // the rule's name, else the layer's title
    GeomType                sample = GeomType::Polygon;   // what the swatch shows
    std::vector<Symbolizer> symbols;    // in drawing order, scaled for the paper
};
struct LegendGroup {
    std::string             title;      // the layer's title
    std::vector<LegendLine> lines;      // one line only: the title is the line's label
};
// With `drawn` (the render's report), only the rules that drew a feature
// on the page: the legend explains the page, not the whole style (the OSM
// roads have 13 kinds; a district shows 4 of them).
std::vector<LegendGroup> legendEntries(const Map& map, const View& view, double pixelRatio,
                                       const MapReport* drawn = nullptr);

// "parcels: Evere, Schaerbeek, (other); roads", for the report.
std::string legendText(const std::vector<LegendGroup>& groups);

// §74: the scale bar's length: a round number of metres of ground (1, 2, 5
// x 10^n) about a fifth of the frame, and its length on the paper in points.
struct ScaleBar { double ground = 0, points = 0; };
ScaleBar scaleBar(const PagePlan& plan);

} // namespace r2d
