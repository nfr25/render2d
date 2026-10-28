// SPDX-License-Identifier: AGPL-3.0-or-later
// The SLD model, reduced: Style > FeatureTypeStyle (one pass) >
// Rule (when: scale, filter) > Symbolizer (how). No YAML here: readers build
// this model. See notions §22 to §25, §28 and §35.
#pragma once

#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "canvas/style.hpp"
#include "style/expression.hpp"

namespace r2d {

// notions §24: applies when min <= denominator < max. "min"/"max" in YSLD
// mean no limit: 0 and infinity here.
struct ScaleRange {
    double min = 0.0;
    double max = std::numeric_limits<double>::infinity();

    bool contains(double denominator) const { return denominator >= min && denominator < max; }
};

// notions §40, §43: the well-known marks of SLD, and the line shapes of
// GeoServer (shape://...) used mostly to hatch polygons.
enum class MarkShape { Circle, Square, Triangle, Star, Cross, X,
                       Slash, Backslash, HorLine, VertLine, Times, Dot };

// A polygon filled with a repeated tile (hatching). §43.
struct GraphicFill {
    double                     size  = 8.0;              // the tile, pixels
    MarkShape                  shape = MarkShape::Slash;
    std::optional<FillStyle>   fill{};                   // for the closed marks
    std::optional<StrokeStyle> stroke{};
};

// A missing fill or stroke means: do not fill / do not stroke.
struct PolygonSymbolizer {
    std::optional<FillStyle>   fill{};
    std::optional<StrokeStyle> stroke{};
    std::optional<GraphicFill> graphicFill{};   // drawn over the fill, under the outline
};

// Applied to lines, and to the outline of polygons (as in SLD).
struct LineSymbolizer {
    StrokeStyle stroke;
};

// A mark (or an image) of `size` pixels, applied to points. §40 to §42.
struct PointSymbolizer {
    double                     size = 6.0;     // pixels (metres with `metres`); 0 = an image's own size
    std::optional<FillStyle>   fill{};
    std::optional<StrokeStyle> stroke{};
    MarkShape                  shape = MarkShape::Circle;
    std::string                image{};        // a PNG, path resolved; drawn first, the mark is the fallback
    std::optional<Expression>  sizeExpr{};     // size: ${4 * sqrt(N)}
    std::optional<Expression>  rotation{};     // degrees, clockwise (SLD): ${90 - ANGLE}
    bool                       metres = false; // uom: metre, §41
};

// A light outline around each glyph, so the text reads on any background. §35.
struct Halo {
    double radius = 1.0;                       // pixels outside the glyph
    Color  color  = Color::fromHex(0xFFFFFF);
};

// A label: text taken from the attributes, drawn after all the layers. §35.
struct TextSymbolizer {
    std::optional<Expression> label;           // ${POLICENUM}, or a fixed text; none = nothing
    FontStyle                 font;
    Color                     fill = Color::fromHex(0x000000);
    std::optional<Halo>       halo;
    double                    anchorX = 0.0;   // SLD default: the text starts at the point...
    double                    anchorY = 0.5;   // ...vertically centred on it
    double                    displacementX = 0.0, displacementY = 0.0;   // pixels, y up as in SLD
    std::optional<Expression> rotation;        // degrees, clockwise (SLD); ${360 - CARTOANGLE}
};

// A raster layer's pixels, faded by `opacity` (0 to 1). §66. Ignored by the
// vector layers, as the other symbolizers are by a raster.
struct RasterSymbolizer {
    double opacity = 1.0;
};

using Symbolizer = std::variant<PolygonSymbolizer, LineSymbolizer, PointSymbolizer, TextSymbolizer,
                                RasterSymbolizer>;

struct Rule {
    std::string             name;
    ScaleRange              scale;
    std::optional<Expression> filter;        // a condition; none = every feature
    bool                    isElse  = false; // else: true, notions §28
    bool                    invalid = false; // unreadable filter: the rule never applies
    std::vector<Symbolizer> symbolizers;
    std::string             invalidFilter{}; // the unreadable filter's text: written back as it was (§59)
};

// One rendering pass over the whole layer. notions §25.
struct FeatureTypeStyle {
    std::vector<Rule> rules;
};

struct Style {
    std::string                   name;
    std::vector<FeatureTypeStyle> featureStyles;
};

// The rules of one pass that apply at this scale, in file order.
inline std::vector<const Rule*> activeRules(const FeatureTypeStyle& fts, double scaleDenominator) {
    std::vector<const Rule*> out;
    for (const Rule& r : fts.rules) {
        if (!r.invalid && r.scale.contains(scaleDenominator)) out.push_back(&r);
    }
    return out;
}

} // namespace r2d
