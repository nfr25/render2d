// SPDX-License-Identifier: AGPL-3.0-or-later
#include "backend/cairo/cairo_target.hpp"

#include <algorithm>
#include <cctype>

#include <cairo.h>
#include <cairo-pdf.h>
#include <cairo-svg.h>

namespace r2d {

std::string cairoVersion() { return cairo_version_string(); }

std::optional<OutputFormat> formatFromPath(const std::string& path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return std::nullopt;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (ext == "png") return OutputFormat::Png;
    if (ext == "svg") return OutputFormat::Svg;
    if (ext == "pdf") return OutputFormat::Pdf;
    return std::nullopt;
}

std::unique_ptr<CairoTarget> CairoTarget::create(const std::string& path, ImageSize size,
                                                 std::string& error) {
    const auto format = formatFromPath(path);
    if (!format) {
        error = "unknown output format for '" + path + "' (expected .png, .svg or .pdf)";
        return nullptr;
    }
    if (size.width <= 0 || size.height <= 0) {
        error = "image size must be positive";
        return nullptr;
    }

    std::unique_ptr<CairoTarget> t(new CairoTarget());
    t->path_   = path;
    t->format_ = *format;

    switch (*format) {
        case OutputFormat::Png:
            // 4 bytes per pixel, transparent background possible. notions §13.
            t->surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size.width, size.height);
            break;
        case OutputFormat::Svg:
            t->surface_ = cairo_svg_surface_create(path.c_str(), size.width, size.height);
            break;
        case OutputFormat::Pdf:
            t->surface_ = cairo_pdf_surface_create(path.c_str(), size.width, size.height);
            break;
    }

    if (cairo_surface_status(t->surface_) != CAIRO_STATUS_SUCCESS) {
        error = std::string("cannot create surface: ") +
                cairo_status_to_string(cairo_surface_status(t->surface_));
        return nullptr;   // destructor releases the (error) surface
    }

    t->cr_     = cairo_create(t->surface_);
    t->canvas_ = std::make_unique<CairoCanvas>(t->cr_);
    return t;
}

bool CairoTarget::finish(std::string& error) {
    if (finished_) return true;
    finished_ = true;

    if (cairo_status(cr_) != CAIRO_STATUS_SUCCESS) {
        error = std::string("drawing failed: ") + cairo_status_to_string(cairo_status(cr_));
        return false;
    }
    if (format_ == OutputFormat::Png) {
        const cairo_status_t s = cairo_surface_write_to_png(surface_, path_.c_str());
        if (s != CAIRO_STATUS_SUCCESS) {
            error = "cannot write '" + path_ + "': " + cairo_status_to_string(s);
            return false;
        }
        return true;
    }
    // SVG / PDF: the file is written as the surface is finished.
    cairo_surface_finish(surface_);
    if (cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS) {
        error = "cannot write '" + path_ + "': " +
                cairo_status_to_string(cairo_surface_status(surface_));
        return false;
    }
    return true;
}

CairoTarget::~CairoTarget() {
    canvas_.reset();
    if (cr_)      cairo_destroy(cr_);
    if (surface_) cairo_surface_destroy(surface_);
}

} // namespace r2d
