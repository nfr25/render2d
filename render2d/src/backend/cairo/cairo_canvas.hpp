// SPDX-License-Identifier: AGPL-3.0-or-later
// Canvas implemented with Cairo. The cairo context keeps an identity matrix:
// we hand it pixels, so line widths stay in pixels. notions §12.
// Text uses Cairo's "toy" API (cairo_text_path). notions §34.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "canvas/canvas.hpp"

typedef struct _cairo cairo_t;   // forward declarations, keep cairo.h out of users
typedef struct _cairo_surface cairo_surface_t;
typedef struct _cairo_pattern cairo_pattern_t;

namespace r2d {

class CairoCanvas final : public Canvas {
public:
    explicit CairoCanvas(cairo_t* cr);   // does not take ownership
    ~CairoCanvas() override;             // frees the images and patterns it loaded

    void clear(const Color& color) override;

    void moveTo(Point p) override;
    void lineTo(Point p) override;
    void curveTo(Point c1, Point c2, Point p) override;
    void closePath() override;

    TextExtents measureText(const std::string& utf8, const FontStyle& font) override;
    void addText(const std::string& utf8, const FontStyle& font, const Affine& placement) override;

    int loadImage(const std::string& path, std::string& error) override;
    ImageInfo imageInfo(int handle) const override;
    void drawImage(int handle, const Affine& placement) override;
    void drawPixels(const PixelBlock& block, int x, int y, double opacity) override;
    int createPattern(int size, const std::function<void(Canvas&)>& drawTile) override;
    void fillPattern(int handle, Point origin, PathMode mode) override;

    void fill(const FillStyle& style, PathMode mode) override;
    void stroke(const StrokeStyle& style, PathMode mode) override;

    void save() override;
    void restore() override;

private:
    cairo_t*  cr_;
    std::vector<cairo_surface_t*>     images_;      // handle = index
    std::map<std::string, int>        imageByPath_;
    std::vector<cairo_pattern_t*>     patterns_;    // handle = index
    FontStyle font_;            // the font last selected, to avoid selecting it again
    bool      fontSet_ = false;

    void selectFont(const FontStyle& font);
};

} // namespace r2d
