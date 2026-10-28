// SPDX-License-Identifier: AGPL-3.0-or-later
// Drawing a whole layer with its style: passes, active rules, filters,
// symbolizers, and the labels to draw at the end.
// See notions §24 (scale ranges), §25 (drawing order), §28 to §30 (filters), §35 (labels).
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "canvas/canvas.hpp"
#include "core/feature_source.hpp"
#include "core/view.hpp"
#include "render/labels.hpp"
#include "style/style.hpp"

namespace r2d {

struct RuleCount {
    std::string name;            // the rule's name, or "pass 1 rule 2"
    std::size_t drawn = 0;       // features the rule drew, all passes
};

struct LayerRenderStats {
    ReadStats read;              // of one pass (every pass reads the same features)
    int       passes   = 0;      // passes actually read (passes with no active rule are skipped)
    double    readMs   = 0.0;    // GDAL reading + conversion, all passes
    double    filterMs = 0.0;    // filter evaluation, all passes
    double    drawMs   = 0.0;    // symbolizers + canvas, all passes

    // Filled only when the style has filters, else rules or labels:
    bool                     filtered = false;
    std::vector<RuleCount>   rules;          // active rules, in file order
    std::vector<std::string> fieldsRead;     // §30: the only columns GDAL decoded
    std::vector<std::string> missingFields;  // named in a filter, absent from the layer
    std::size_t              notNumber = 0;  // §28: values that could not be read as numbers
    std::size_t              labels      = 0; // §35: labels collected
    std::size_t              labelsEmpty = 0; // text symbolizers with a NULL or empty text
    std::vector<std::string> imageErrors;    // §42: images that could not be read (marks drawn instead)

    // A raster layer (§65, §66): read.read and read.drawn stay 0.
    bool   raster = false;
    int    pixelsWidth = 0, pixelsHeight = 0;   // the image pixels drawn
    double fileWidth = 0.0, fileHeight = 0.0;   // the file pixels they cover
    bool   reduced = false;                     // averaged (else the nearest file pixel)
};

// The style to use for each geometry type, indexed by GeomType (Point, Line,
// Polygon). A style file gives the same Style for the three; the default
// styles differ per type.
using StyleByType = std::array<const Style*, 3>;

// Labels are appended to `labels`, to be drawn after all the layers (§35).
// Without a collector, they are drawn at the end of this layer.
LayerRenderStats renderLayer(FeatureSource& source, Canvas& cv, const View& view,
                             const StyleByType& styles, std::vector<Label>* labels = nullptr);

} // namespace r2d
