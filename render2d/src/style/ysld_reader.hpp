// SPDX-License-Identifier: AGPL-3.0-or-later
// YSLD (the YAML form of SLD, as defined by GeoServer) -> Style.
// The only file that includes yaml-cpp. See notions §23.
//
// "Always a result": unknown keys and unreadable values are reported as
// warnings (with their line) and skipped. Only a file that is not valid YAML,
// or not a YAML map, gives no style at all.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "style/style.hpp"

namespace r2d {

struct YsldResult {
    std::optional<Style>     style;     // empty only when the YAML cannot be read
    std::vector<std::string> warnings;  // "line 7: unknown key 'stroke-widht' in polygon (ignored)"
    std::string              error;     // set when style is empty
};

// `baseDir`: the folder that relative image paths are read from (the style file's).
YsldResult parseYsld(const std::string& text, const std::string& baseDir = "");
YsldResult readYsldFile(const std::string& path);

} // namespace r2d
