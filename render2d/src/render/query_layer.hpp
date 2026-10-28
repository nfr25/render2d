// SPDX-License-Identifier: AGPL-3.0-or-later
// "What is here?" for one layer: the features a point touches, among those
// the style draws at this scale. Uses the same RuleMatcher as renderLayer.
// See notions §50 to §52.
#pragma once

#include <cstddef>
#include <vector>

#include "core/attributes.hpp"
#include "core/feature_source.hpp"
#include "core/geometry.hpp"
#include "core/view.hpp"
#include "render/render_layer.hpp"

namespace r2d {

inline constexpr std::size_t kNoLimit = static_cast<std::size_t>(-1);

struct LayerHit {
    FeatureId  fid = 0;
    Geometry   geometry;     // a copy
    Attributes attributes;   // every field of the layer
};

// `world`: the point, in map units. `tolerancePx`: image pixels (§52).
// The features touched, the one drawn on top first (the last read).
std::vector<LayerHit> queryLayer(FeatureSource& source, const View& view, Point world, double tolerancePx,
                                 const StyleByType& styles);

// "What is in this zone?" (§54): the features drawn at this scale whose
// GEOMETRY shares a point with `zone` (one ring, map units). No symbol, no
// tolerance. At most `maxHits` (kNoLimit: no limit): one more feature, and
// nothing more is copied, `truncated` becomes true. The one drawn on top first.
std::vector<LayerHit> queryLayerArea(FeatureSource& source, const View& view, const Geometry& zone,
                                     const StyleByType& styles, std::size_t maxHits, bool& truncated);

} // namespace r2d
