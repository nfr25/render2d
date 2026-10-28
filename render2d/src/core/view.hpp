// SPDX-License-Identifier: AGPL-3.0-or-later
// A View answers: "which part of the world, on how many pixels?"
// It turns a requested envelope and an image size into the world -> image
// transform, keeping pixels square (notions §3, §4) and giving the map
// scale (notions §7).
#pragma once

#include <optional>

#include "core/affine.hpp"
#include "core/envelope.hpp"

namespace r2d {

// OGC "standardized rendering pixel": 0.28 mm. notions §7.
inline constexpr double kStandardPixelSizeM = 0.00028;

struct ImageSize {
    int width  = 0;
    int height = 0;
};

struct View {
    Envelope  requested;      // what the caller asked for
    Envelope  shown;          // what the image really covers (may be wider)
    ImageSize size;
    double    resolution = 0; // metres per pixel, same in x and y
    bool      widened = false;// true when shown != requested (aspect ratio)

    Affine worldToImage;      // metres -> pixels
    Affine imageToWorld;      // pixels -> metres

    // Image pixels per pixel seen on screen: 1.5 on a screen scaled to 150 %
    // (the image is shown 1.5 times smaller). notions §39.
    double pixelRatio = 1.0;

    // Map scale denominator, for the pixels the reader sees: 17857 means 1:17857.
    double scaleDenominator() const { return resolution * pixelRatio / kStandardPixelSizeM; }
};

// Empty when the envelope has no area or the image has no pixel.
// The caller decides how to report it (core stays free of I/O).
std::optional<View> makeView(const Envelope& requested, ImageSize size);

} // namespace r2d
