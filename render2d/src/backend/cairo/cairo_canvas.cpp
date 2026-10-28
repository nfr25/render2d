// SPDX-License-Identifier: AGPL-3.0-or-later
#include "backend/cairo/cairo_canvas.hpp"

#include <cairo.h>

#include <cstring>

namespace r2d {

namespace {

void setSource(cairo_t* cr, const Color& c) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

cairo_fill_rule_t toCairo(FillRule r) {
    return r == FillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING;
}

cairo_line_cap_t toCairo(LineCap c) {
    switch (c) {
        case LineCap::Round:  return CAIRO_LINE_CAP_ROUND;
        case LineCap::Square: return CAIRO_LINE_CAP_SQUARE;
        case LineCap::Butt:   break;
    }
    return CAIRO_LINE_CAP_BUTT;
}

cairo_line_join_t toCairo(LineJoin j) {
    switch (j) {
        case LineJoin::Round: return CAIRO_LINE_JOIN_ROUND;
        case LineJoin::Bevel: return CAIRO_LINE_JOIN_BEVEL;
        case LineJoin::Miter: break;
    }
    return CAIRO_LINE_JOIN_MITER;
}

} // namespace

CairoCanvas::CairoCanvas(cairo_t* cr) : cr_(cr) {}

CairoCanvas::~CairoCanvas() {
    for (cairo_surface_t* s : images_)   cairo_surface_destroy(s);
    for (cairo_pattern_t* p : patterns_) cairo_pattern_destroy(p);
}

// --- images: notions §42 ---------------------------------------------------

int CairoCanvas::loadImage(const std::string& path, std::string& error) {
    if (auto it = imageByPath_.find(path); it != imageByPath_.end()) {
        if (it->second < 0) error = "cannot read image '" + path + "'";
        return it->second;
    }
    cairo_surface_t* s = cairo_image_surface_create_from_png(path.c_str());
    int handle = -1;
    if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
        images_.push_back(s);
        handle = static_cast<int>(images_.size() - 1);
    } else {
        error = "cannot read image '" + path + "': " + cairo_status_to_string(cairo_surface_status(s));
        cairo_surface_destroy(s);
    }
    imageByPath_[path] = handle;   // a missing file is not tried again for every point
    return handle;
}

ImageInfo CairoCanvas::imageInfo(int handle) const {
    if (handle < 0 || handle >= static_cast<int>(images_.size())) return {};
    return {cairo_image_surface_get_width(images_[handle]), cairo_image_surface_get_height(images_[handle])};
}

void CairoCanvas::drawImage(int handle, const Affine& m) {
    if (handle < 0 || handle >= static_cast<int>(images_.size())) return;
    cairo_save(cr_);
    cairo_matrix_t cm;
    cairo_matrix_init(&cm, m.a, m.b, m.c, m.d, m.e, m.f);
    cairo_transform(cr_, &cm);
    cairo_set_source_surface(cr_, images_[handle], 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_GOOD);   // smooth when scaled
    cairo_paint(cr_);
    cairo_restore(cr_);
}

// --- pixels: notions §66 ------------------------------------------------------

void CairoCanvas::drawPixels(const PixelBlock& block, int x, int y, double opacity) {
    if (block.width <= 0 || block.height <= 0 ||
        block.argb.size() < static_cast<std::size_t>(block.width) * static_cast<std::size_t>(block.height))
        return;
    // A copy in a surface Cairo owns: an SVG or PDF keeps it until the page
    // is written, after the block is gone.
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, block.width, block.height);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); return; }
    cairo_surface_flush(s);
    unsigned char* data = cairo_image_surface_get_data(s);
    const int stride = cairo_image_surface_get_stride(s);
    for (int row = 0; row < block.height; ++row)
        std::memcpy(data + static_cast<std::size_t>(row) * stride,
                    block.argb.data() + static_cast<std::size_t>(row) * block.width,
                    static_cast<std::size_t>(block.width) * 4);
    cairo_surface_mark_dirty(s);
    cairo_save(cr_);
    cairo_set_source_surface(cr_, s, x, y);
    cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_NEAREST);   // one pixel = one pixel
    cairo_rectangle(cr_, x, y, block.width, block.height);
    cairo_clip(cr_);
    if (opacity >= 1.0) cairo_paint(cr_);
    else                cairo_paint_with_alpha(cr_, opacity);
    cairo_restore(cr_);
    cairo_surface_destroy(s);   // Cairo keeps its own reference while it needs it
}

// --- repeated tiles: notions §43 ---------------------------------------------

int CairoCanvas::createPattern(int size, const std::function<void(Canvas&)>& drawTile) {
    if (size < 1) size = 1;
    cairo_surface_t* tile = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* tcr = cairo_create(tile);
    {
        CairoCanvas tc(tcr);
        tc.clear(Color{0.0, 0.0, 0.0, 0.0});   // transparent: the polygon's fill shows through
        drawTile(tc);
    }
    cairo_destroy(tcr);
    cairo_pattern_t* p = cairo_pattern_create_for_surface(tile);
    cairo_surface_destroy(tile);               // the pattern keeps its own reference
    cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
    cairo_pattern_set_filter(p, CAIRO_FILTER_NEAREST);   // tile pixels as drawn, no blur
    patterns_.push_back(p);
    return static_cast<int>(patterns_.size() - 1);
}

void CairoCanvas::fillPattern(int handle, Point origin, PathMode mode) {
    if (handle < 0 || handle >= static_cast<int>(patterns_.size())) return;
    cairo_pattern_t* p = patterns_[handle];
    cairo_matrix_t pm;                          // user space -> pattern space
    cairo_matrix_init_translate(&pm, -origin.x, -origin.y);
    cairo_pattern_set_matrix(p, &pm);
    cairo_set_source(cr_, p);
    cairo_set_fill_rule(cr_, CAIRO_FILL_RULE_EVEN_ODD);
    if (mode == PathMode::Keep) cairo_fill_preserve(cr_);
    else                        cairo_fill(cr_);
}

void CairoCanvas::clear(const Color& color) {
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_SOURCE);  // replace, do not blend
    setSource(cr_, color);
    cairo_paint(cr_);
    cairo_restore(cr_);
}

void CairoCanvas::moveTo(Point p)  { cairo_move_to(cr_, p.x, p.y); }
void CairoCanvas::lineTo(Point p)  { cairo_line_to(cr_, p.x, p.y); }
void CairoCanvas::closePath()      { cairo_close_path(cr_); }

void CairoCanvas::curveTo(Point c1, Point c2, Point p) {
    cairo_curve_to(cr_, c1.x, c1.y, c2.x, c2.y, p.x, p.y);
}

void CairoCanvas::selectFont(const FontStyle& font) {
    if (fontSet_ && font.family == font_.family && font.size == font_.size &&
        font.bold == font_.bold && font.italic == font_.italic) return;
    cairo_select_font_face(cr_, font.family.c_str(),
                           font.italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
                           font.bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr_, font.size);
    font_ = font;
    fontSet_ = true;
}

TextExtents CairoCanvas::measureText(const std::string& utf8, const FontStyle& font) {
    selectFont(font);
    cairo_text_extents_t e;
    cairo_text_extents(cr_, utf8.c_str(), &e);
    return {e.x_bearing, e.y_bearing, e.width, e.height, e.x_advance};
}

void CairoCanvas::addText(const std::string& utf8, const FontStyle& font, const Affine& m) {
    selectFont(font);
    // The path is built under the placement matrix, then the identity comes
    // back: Cairo keeps paths in device space, so strokes stay in pixels.
    cairo_save(cr_);
    cairo_matrix_t cm;
    cairo_matrix_init(&cm, m.a, m.b, m.c, m.d, m.e, m.f);
    cairo_transform(cr_, &cm);
    cairo_move_to(cr_, 0.0, 0.0);
    cairo_text_path(cr_, utf8.c_str());
    cairo_restore(cr_);
}

void CairoCanvas::fill(const FillStyle& style, PathMode mode) {
    setSource(cr_, style.color);
    cairo_set_fill_rule(cr_, toCairo(style.rule));
    if (mode == PathMode::Keep) cairo_fill_preserve(cr_);
    else                        cairo_fill(cr_);
}

void CairoCanvas::stroke(const StrokeStyle& style, PathMode mode) {
    setSource(cr_, style.color);
    cairo_set_line_width(cr_, style.width);
    cairo_set_line_cap(cr_, toCairo(style.cap));
    cairo_set_line_join(cr_, toCairo(style.join));
    cairo_set_dash(cr_, style.dashes.data(), static_cast<int>(style.dashes.size()), style.dashOffset);
    if (mode == PathMode::Keep) cairo_stroke_preserve(cr_);
    else                        cairo_stroke(cr_);
}

void CairoCanvas::save()    { cairo_save(cr_); }
void CairoCanvas::restore() { cairo_restore(cr_); }

} // namespace r2d
