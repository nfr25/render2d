// SPDX-License-Identifier: AGPL-3.0-or-later
// The drawing vocabulary the renderer speaks. It knows nothing of Cairo:
// Cairo is the first implementation, the home-made engine will be the next.
// Coordinates are image coordinates (pixels for PNG, points for SVG/PDF).
// See notions §10.
#pragma once

#include <functional>
#include <string>

#include "canvas/style.hpp"
#include "core/affine.hpp"
#include "core/point.hpp"

namespace r2d {

// What happens to the current path after fill() or stroke().
// Keep = fill then stroke the same path (cairo_fill_preserve in the BIM engine).
enum class PathMode { Consume, Keep };

class Canvas {
public:
    virtual ~Canvas() = default;

    Canvas() = default;
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    // --- background ----------------------------------------------------
    // Replaces every pixel, alpha included (a = 0 gives a transparent image).
    virtual void clear(const Color& color) = 0;

    // --- path construction (nothing is painted yet) --------------------
    virtual void moveTo(Point p) = 0;
    virtual void lineTo(Point p) = 0;
    virtual void curveTo(Point c1, Point c2, Point p) = 0;  // cubic Bezier
    virtual void closePath() = 0;

    // --- text: glyphs are outlines, added to the path like the rest. §33, §34
    // The ink box of `utf8` set in `font`.
    virtual TextExtents measureText(const std::string& utf8, const FontStyle& font) = 0;
    // Adds the glyph outlines to the current path. The text is laid out with
    // its baseline starting at (0, 0), x to the right, y down, then mapped
    // by `placement` (rotation, anchor...) into image coordinates.
    virtual void addText(const std::string& utf8, const FontStyle& font, const Affine& placement) = 0;

    // --- images: notions §42 ---------------------------------------------
    // Loads a PNG once (same path, same handle). Returns -1 and sets `error`
    // when it cannot be read.
    virtual int loadImage(const std::string& path, std::string& error) = 0;
    virtual ImageInfo imageInfo(int handle) const = 0;
    // Paints the image, its pixels mapped by `placement` into image coordinates.
    virtual void drawImage(int handle, const Affine& placement) = 0;

    // --- pixels: notions §66 -------------------------------------------------
    // Paints the block with its top-left corner on image pixel (x, y), one
    // block pixel per image pixel, faded by `opacity` (0 to 1).
    virtual void drawPixels(const PixelBlock& block, int x, int y, double opacity) = 0;

    // --- repeated tiles (hatching): notions §43 ---------------------------
    // A size x size tile drawn once by `drawTile`, then repeated.
    virtual int createPattern(int size, const std::function<void(Canvas&)>& drawTile) = 0;
    // Fills the current path with the tile, the tiling starting at `origin`.
    virtual void fillPattern(int handle, Point origin, PathMode mode) = 0;

    // --- painting ------------------------------------------------------
    virtual void fill(const FillStyle& style, PathMode mode) = 0;
    virtual void stroke(const StrokeStyle& style, PathMode mode) = 0;

    // --- state ---------------------------------------------------------
    virtual void save() = 0;
    virtual void restore() = 0;

    // Convenience: fill, then stroke the same path, then forget it.
    void fillAndStroke(const FillStyle& f, const StrokeStyle& s) {
        fill(f, PathMode::Keep);
        stroke(s, PathMode::Consume);
    }
};

} // namespace r2d
