// SPDX-License-Identifier: AGPL-3.0-or-later
// Labels: collected while the layers are drawn, then drawn all together,
// above every layer. See notions §35 to §37.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "canvas/canvas.hpp"
#include "core/attributes.hpp"
#include "core/geometry.hpp"
#include "style/style.hpp"

namespace r2d {

// One label, ready to draw: everything is already in image pixels.
struct Label {
    std::string         text;
    FontStyle           font;
    Color               fill;
    std::optional<Halo> halo;
    Point               at;                    // the anchor point on the image
    double              anchorX = 0.0, anchorY = 0.5;
    double              dx = 0.0, dy = 0.0;    // displacement, pixels, y up (SLD)
    double              rotation = 0.0;        // degrees clockwise, already made readable
};

// The text of a value: NULL gives "", 120.0 gives "120", 120.5 gives "120.5".
std::string labelText(const Value& v);

// notions §36: an angle in [0, 360), turned by 180° when the text would be
// upside down (between 90° excluded and 270° included). 228.24 -> 48.24.
double readableAngle(double clockwiseDegrees);

// Where the label of a geometry goes, in world coordinates: each point of a
// (multi)point, the middle of a line's middle segment, the centre of a
// polygon's envelope. Simple on purpose: placement comes with collisions.
std::vector<Point> labelPoints(const Geometry& g);

// Text space -> image: baseline at (0, 0) -> anchor on the point, rotated.
Affine labelPlacement(const Label& l, const TextExtents& ink);

// Draws the labels in order (halo first, then the glyphs). Returns how many were drawn.
std::size_t drawLabels(Canvas& cv, const std::vector<Label>& labels);

} // namespace r2d
