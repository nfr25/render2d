// SPDX-License-Identifier: AGPL-3.0-or-later
// Drawing one Geometry with one symbolizer. Knows the canvas and the style
// model, not Cairo, GDAL nor YAML. See notions §19 and §22.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "canvas/canvas.hpp"
#include "core/affine.hpp"
#include "core/geometry.hpp"
#include "style/style.hpp"

namespace r2d {

// Which geometries each symbolizer draws (as in SLD):
//   PolygonSymbolizer -> polygons: all rings in one path, even-odd fill, outline
//   LineSymbolizer    -> lines, and the outline of polygons
//   PointSymbolizer   -> points: a mark or a PNG of `size` pixels (or metres)
//   TextSymbolizer    -> nothing here: renderLayer collects the labels (§35)
// Any other combination draws nothing.
// What a symbolizer may need beyond the geometry, per feature or per render.
struct SymbolContext {
    double                     size     = -1.0;   // from PointSymbolizer::sizeExpr; < 0 = the fixed size
    double                     rotation = 0.0;    // degrees clockwise, from PointSymbolizer::rotation
    std::map<const void*, int>* patterns = nullptr;   // hatching tiles made once per render (§43)
    std::vector<std::string>*  errors   = nullptr;    // images that could not be read (§42)
};

void drawSymbolizer(Canvas& cv, const Affine& worldToImage, const Geometry& g,
                    const Symbolizer& sym, const SymbolContext& ctx = {});

// How far (pixels) a symbolizer can reach beyond its geometry: half the
// stroke width, or the point radius plus half its outline. notions §18.
double symbolizerReachPx(const Symbolizer& sym);
double styleReachPx(const Style& style);

// notions §39: the same style for an image seen `ratio` times smaller: every
// length in pixels (widths, dashes, sizes, fonts, halos, displacements) times `ratio`.
Style scaledStyle(const Style& style, double ratio);

// The step 2 look as a Style: one pass, one rule, chosen from the geometry
// type and the layer's position (so parcels and buildings differ).
Style defaultStyle(GeomType type, int layerIndex);

} // namespace r2d
