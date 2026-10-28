// SPDX-License-Identifier: AGPL-3.0-or-later
// The bridge between pipeline step 4 (world -> image) and step 5 (canvas):
// world vertices go through the Affine here, the canvas only sees pixels.
// See notions §12.
#pragma once

#include <span>

#include "canvas/canvas.hpp"
#include "core/affine.hpp"

namespace r2d {

// Adds one ring (closed) or one line (open) to the current path.
// Fewer than 2 points: nothing to draw, nothing added.
inline void addWorldPath(Canvas& canvas, const Affine& worldToImage,
                         std::span<const Point> world, bool closed) {
    if (world.size() < 2) return;
    canvas.moveTo(worldToImage.apply(world[0]));
    for (std::size_t i = 1; i < world.size(); ++i) {
        canvas.lineTo(worldToImage.apply(world[i]));
    }
    if (closed) canvas.closePath();
}

} // namespace r2d
