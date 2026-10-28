// SPDX-License-Identifier: AGPL-3.0-or-later
// An output file and its Cairo surface: PNG, SVG or PDF, chosen from the
// file extension. Same drawing code for the three. notions §13.
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "backend/cairo/cairo_canvas.hpp"
#include "core/view.hpp"

typedef struct _cairo_surface cairo_surface_t;

namespace r2d {

enum class OutputFormat { Png, Svg, Pdf };

// ".png" / ".svg" / ".pdf", case-insensitive; empty for anything else.
std::optional<OutputFormat> formatFromPath(const std::string& path);

// "1.18.0": for --version.
std::string cairoVersion();

class CairoTarget {
public:
    // Size is in pixels for PNG, in points for SVG and PDF.
    // Returns nullptr and fills `error` when the surface cannot be created.
    static std::unique_ptr<CairoTarget> create(const std::string& path, ImageSize size,
                                               std::string& error);
    ~CairoTarget();

    CairoTarget(const CairoTarget&) = delete;
    CairoTarget& operator=(const CairoTarget&) = delete;

    Canvas&      canvas() { return *canvas_; }
    OutputFormat format() const { return format_; }

    // Writes the file (PNG) or completes it (SVG, PDF). Call once, at the end.
    bool finish(std::string& error);

private:
    CairoTarget() = default;

    std::string                  path_;
    OutputFormat                 format_  = OutputFormat::Png;
    cairo_surface_t*             surface_ = nullptr;
    cairo_t*                     cr_      = nullptr;
    std::unique_ptr<CairoCanvas> canvas_;
    bool                         finished_ = false;
};

} // namespace r2d
