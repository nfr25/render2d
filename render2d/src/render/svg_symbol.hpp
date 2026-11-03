// SPDX-License-Identifier: AGPL-3.0-or-later
// SVG symbols (notions §75): read once with nanosvg (one header, no DLL),
// drawn as paths by the canvas, so they stay vector in a PDF and are the
// same in the legend as on the map.
#pragma once

#include <optional>
#include <string>

#include "core/affine.hpp"

namespace r2d {

class Canvas;

// "icons/tree.svg", ".SVG": a file ending in .svg.
bool isSvgFile(const std::string& path);

// The SVG's own size (its width and height, in its units), read once per
// file and kept. Empty, with `error`, when it cannot be read.
struct SvgSize { double width = 0, height = 0; };
std::optional<SvgSize> svgSize(const std::string& path, std::string& error);

// Draws the SVG: its units mapped to the image by `placement` (position,
// rotation, uniform scale `scale`, the strokes' widths multiplied by it).
// False, with `error`, when the file cannot be read.
bool drawSvg(Canvas& cv, const std::string& path, const Affine& placement, double scale, std::string& error);

} // namespace r2d
