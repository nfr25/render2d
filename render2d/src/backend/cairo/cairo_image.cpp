// SPDX-License-Identifier: AGPL-3.0-or-later
#include "backend/cairo/cairo_image.hpp"

#include <cairo.h>

namespace r2d {

std::unique_ptr<CairoImage> CairoImage::create(ImageSize size, std::string& error) {
    if (size.width <= 0 || size.height <= 0) {
        error = "image size must be positive";
        return nullptr;
    }
    std::unique_ptr<CairoImage> img(new CairoImage());
    img->size_    = size;
    img->surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size.width, size.height);
    if (cairo_surface_status(img->surface_) != CAIRO_STATUS_SUCCESS) {
        error = std::string("cannot create image: ") +
                cairo_status_to_string(cairo_surface_status(img->surface_));
        return nullptr;   // destructor releases the (error) surface
    }
    img->cr_     = cairo_create(img->surface_);
    img->canvas_ = std::make_unique<CairoCanvas>(img->cr_);
    return img;
}

int CairoImage::stride() const {
    return cairo_image_surface_get_stride(surface_);
}

const unsigned char* CairoImage::pixels() {
    cairo_surface_flush(surface_);   // §48: memory is up to date only after this
    return cairo_image_surface_get_data(surface_);
}

CairoImage::~CairoImage() {
    canvas_.reset();
    if (cr_)      cairo_destroy(cr_);
    if (surface_) cairo_surface_destroy(surface_);
}

} // namespace r2d
