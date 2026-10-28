// SPDX-License-Identifier: AGPL-3.0-or-later
// Small value types describing how to paint. They mirror the SLD
// vocabulary (fill, stroke, cap, join, dashes). See notions §10 and §11.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace r2d {

// Colour with straight (non-premultiplied) alpha, components in 0..1.
struct Color {
    double r = 0.0, g = 0.0, b = 0.0, a = 1.0;

    // 0xRRGGBB, as in the BIM engine styles and in CSS.
    static Color fromHex(std::uint32_t rgb, double alpha = 1.0) {
        return {((rgb >> 16) & 0xFF) / 255.0,
                ((rgb >> 8)  & 0xFF) / 255.0,
                ( rgb        & 0xFF) / 255.0,
                alpha};
    }
};

// notions §11: even-odd is the project default, whatever the ring order.
enum class FillRule { EvenOdd, NonZero };

enum class LineCap  { Butt, Round, Square };
enum class LineJoin { Miter, Round, Bevel };

struct FillStyle {
    Color    color{};
    FillRule rule = FillRule::EvenOdd;
};

// Widths and dash lengths are in image units (pixels or points), never in
// metres: that is why vertices are transformed before reaching the canvas.
// notions §12.
struct StrokeStyle {
    Color               color{};
    double              width = 1.0;
    LineCap             cap   = LineCap::Butt;
    LineJoin            join  = LineJoin::Miter;
    std::vector<double> dashes{};        // "on, off, on, off..."; empty = solid
    double              dashOffset = 0.0;
};

// A font, as the style names it. The family is resolved by the system;
// an unknown name falls back to a default font, without error. notions §34.
struct FontStyle {
    std::string family = "Arial";
    double      size   = 10.0;    // pixels: the height of the em
    bool        bold   = false;
    bool        italic = false;
};

// Where the ink of a text lies, relative to the start of its baseline,
// y downwards (so yBearing is negative: the ink rises above the baseline).
// notions §33.
struct TextExtents {
    double xBearing = 0.0, yBearing = 0.0;   // top-left corner of the ink box
    double width    = 0.0, height   = 0.0;   // size of the ink box
    double advance  = 0.0;                   // how far the pen moves
};

// The size of an image loaded by a canvas, in its own pixels. notions §42.
struct ImageInfo {
    int width  = 0;
    int height = 0;
};

// A block of pixels made by the program (a raster, §66): premultiplied
// ARGB32, one std::uint32_t per pixel, 0xAARRGGBB (B, G, R, A in memory on
// a PC), row after row, no padding.
struct PixelBlock {
    int                        width  = 0;
    int                        height = 0;
    std::vector<std::uint32_t> argb;
};

} // namespace r2d
