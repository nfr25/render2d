// SPDX-License-Identifier: AGPL-3.0-or-later
// The map described in a file: viewer2d's viewer.yaml (notions §57).
// One reader for render2d (--config) and editor2d: they cannot understand
// the same file in two ways.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/envelope.hpp"
#include "map/map.hpp"

namespace r2d {

struct Config {
    std::string             path;        // the file read
    std::vector<LayerSpec>  layers;      // drawn in this order, first at the bottom
    std::optional<Envelope> startBbox;   // start: bbox, if given (in the map's CRS)
    std::string             crs;         // §70: "EPSG:3857" as written; "" = the first layer's
};

struct ConfigResult {
    bool                     ok = false;
    Config                   config;
    std::string              error;      // when !ok: why the file cannot be used at all
    std::vector<std::string> warnings;   // unknown keys: often a typo
};

// Reads `crs` (§70: checked, a CRS in metres), `layers` (source, style,
// visible, title) and `start: bbox`. The keys
// of viewer2d (render2d, max_parallel, tiles, metatile) are ignored; any other key is a
// warning. Relative paths are taken from the folder of the file, not from the
// current folder. Missing data files are NOT checked here: Map::open reports
// them, and the other layers are drawn.
ConfigResult loadConfig(const std::string& path);

// The text writeConfig would write to `path` (§61): the layers, then start:
// bbox, then the keys of the file already at `path` that are not ours
// (viewer2d's render2d, tiles...), copied as they are. Comments are lost.
// editor2d compares it with the file to know if the project changed.
std::string configText(const Config& config, const std::string& path);

// Writes configText() to `path`. False and `error` set when it cannot.
bool writeConfig(const Config& config, const std::string& path, std::string& error);

// `path` relative to `folder` when it is inside it ("data/a.fgb"), else as it
// is (§61). "file.gpkg::layer": the file part only; "PG:...": unchanged.
// On Windows, letters case and slash direction do not matter.
std::string relativeToFolder(const std::string& path, const std::string& folder);

} // namespace r2d
