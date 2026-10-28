// SPDX-License-Identifier: AGPL-3.0-or-later
// A Cairo image in memory, whose pixels can be read: for editor2d, which sends
// them to the graphics card instead of writing a file. notions §48.
#pragma once

#include <memory>
#include <string>

#include "backend/cairo/cairo_canvas.hpp"
#include "core/view.hpp"

typedef struct _cairo_surface cairo_surface_t;

namespace r2d {

class CairoImage {
public:
    // ARGB32, premultiplied alpha, native byte order: B, G, R, A in memory on
    // a PC. Returns nullptr and fills `error` when the surface cannot be created.
    static std::unique_ptr<CairoImage> create(ImageSize size, std::string& error);
    ~CairoImage();

    CairoImage(const CairoImage&) = delete;
    CairoImage& operator=(const CairoImage&) = delete;

    Canvas&   canvas() { return *canvas_; }
    ImageSize size() const { return size_; }

    // Bytes from the start of one row to the start of the next. Always ask:
    // it may be more than width x 4.
    int stride() const;

    // Flushes Cairo's pending drawing, then the first byte of row 0 (the top
    // row). Valid until the next drawing or the end of the image.
    const unsigned char* pixels();

private:
    CairoImage() = default;

    ImageSize                    size_;
    cairo_surface_t*             surface_ = nullptr;
    cairo_t*                     cr_      = nullptr;
    std::unique_ptr<CairoCanvas> canvas_;
};

} // namespace r2d
