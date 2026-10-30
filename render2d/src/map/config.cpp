// SPDX-License-Identifier: AGPL-3.0-or-later
#include "map/config.hpp"

#include "backend/gdal/gdal_reproject.hpp"   // crsFromUser (§70)

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace r2d {

namespace {

namespace fs = std::filesystem;

// "styles/a.yaml" next to the config file -> its full path (§57). Absolute
// paths, GDAL connection strings ("PG:...") and "file::layer" keep their form.
std::string resolve(const std::string& value, const fs::path& folder) {
    if (value.rfind("PG:", 0) == 0) return value;
    std::string file = value, layer;
    if (const auto sep = value.rfind("::"); sep != std::string::npos) {
        file  = value.substr(0, sep);
        layer = value.substr(sep);   // keeps the "::"
    }
    // Kept as written: "C:/..." (absolute), "C:data" (root name), and "/data/..."
    // (root directory: absolute on Linux, but NOT on Windows, where it means
    // "the current drive": is_absolute() alone would attach our folder to it).
    const fs::path p(file);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return value;
    return (folder / p).lexically_normal().generic_string() + layer;
}

// Keys of a map node that are not in `known`: "where: 'key' is unknown".
void unknownKeys(const YAML::Node& node, const std::set<std::string>& known, const std::string& where,
                 std::vector<std::string>& warnings) {
    for (const auto& kv : node) {
        const std::string key = kv.first.as<std::string>();
        if (!known.count(key)) warnings.push_back(where + "'" + key + "' is unknown, ignored");
    }
}

// The keys we read and write; any other key of an existing file is kept.
const std::set<std::string> kOurKeys = {"crs", "layers", "start"};

// One path component against another: on Windows "Cartes" is "cartes".
bool samePart(const std::string& a, const std::string& b) {
#ifdef _WIN32
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
#else
    return a == b;
#endif
}

// A YAML string in single quotes ('' for a quote), as the YSLD writer does.
std::string quoted(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += (c == '\'' ? std::string("''") : std::string(1, c));
    return out + "'";
}

std::string number(double x) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, x);
    return std::string(buf, r.ptr);
}

} // namespace

std::string relativeToFolder(const std::string& value, const std::string& folder) {
    if (value.rfind("PG:", 0) == 0 || folder.empty()) return value;
    std::string file = value, layer;
    if (const auto sep = value.rfind("::"); sep != std::string::npos) {
        file  = value.substr(0, sep);
        layer = value.substr(sep);
    }
    std::string f = file, d = folder;
#ifdef _WIN32
    std::replace(f.begin(), f.end(), '\\', '/');
    std::replace(d.begin(), d.end(), '\\', '/');
#endif
    const fs::path p = fs::path(f).lexically_normal();
    const fs::path base = fs::path(d).lexically_normal();
    if (!p.has_root_directory() || !base.has_root_directory()) return value;   // already relative
    // Component by component: the whole folder must be the start of the path.
    auto pi = p.begin();
    for (auto bi = base.begin(); bi != base.end(); ++bi) {
        if (bi->empty()) continue;   // the trailing "" of "D:/maps/"
        if (pi == p.end() || !samePart(pi->string(), bi->string())) return value;
        ++pi;
    }
    fs::path rest;
    for (; pi != p.end(); ++pi) rest /= *pi;
    if (rest.empty()) return value;
    return rest.generic_string() + layer;
}

std::string configText(const Config& config, const std::string& path) {
    const std::string folder = fs::absolute(fs::path(path)).parent_path().generic_string();
    std::ostringstream o;
    o << "# Written by render2d's writeConfig (notions §61). Paths are relative to this file.\n";
    if (!config.crs.empty()) o << "crs: " << quoted(config.crs) << "\n";   // §70
    o << "layers:\n";
    for (const LayerSpec& l : config.layers) {
        o << "  - source: " << quoted(relativeToFolder(l.source, folder)) << "\n";
        if (l.style && !l.style->empty()) o << "    style: " << quoted(relativeToFolder(*l.style, folder)) << "\n";
        if (!l.visible) o << "    visible: false\n";
        if (!l.title.empty()) o << "    title: " << quoted(l.title) << "\n";
        if (!l.crs.empty()) o << "    crs: " << quoted(l.crs) << "\n";   // §70
    }
    if (config.startBbox) {
        const Envelope& b = *config.startBbox;
        o << "start:\n  bbox: [" << number(b.xmin) << ", " << number(b.ymin) << ", " << number(b.xmax) << ", "
          << number(b.ymax) << "]\n";
    }
    // The other keys of the file already there (viewer2d's), in their order.
    try {
        const YAML::Node old = YAML::LoadFile(path);
        if (old.IsMap()) {
            YAML::Node others(YAML::NodeType::Map);
            for (const auto& kv : old)
                if (!kOurKeys.count(kv.first.as<std::string>())) others[kv.first] = kv.second;
            if (others.size() > 0) o << YAML::Dump(others) << "\n";
        }
    } catch (const YAML::Exception&) {
        // No file yet, or not readable: nothing to keep.
    }
    return o.str();
}

bool writeConfig(const Config& config, const std::string& path, std::string& error) {
    const std::string text = configText(config, path);
    std::ofstream file(path, std::ios::binary);
    file << text;
    if (!file) {
        error = "cannot write '" + path + "'";
        return false;
    }
    return true;
}

ConfigResult loadConfig(const std::string& path) {
    ConfigResult result;
    result.config.path = path;

    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::BadFile&) {
        result.error = "cannot read '" + path + "'";
        return result;
    } catch (const YAML::Exception& e) {
        result.error = path + " is not valid YAML: " + e.what();
        return result;
    }
    if (!root.IsMap()) {
        result.error = path + ": expected a map (layers, start)";
        return result;
    }
    // viewer2d's own keys are not ours, but they are not mistakes either.
    unknownKeys(root, {"crs", "layers", "start", "render2d", "max_parallel", "tiles", "metatile"}, path + ": ", result.warnings);

    const fs::path folder = fs::absolute(fs::path(path)).parent_path();

    // crs: the map's system (§70), optional. Checked now: a typo stops here.
    if (const YAML::Node crs = root["crs"]) {
        std::string error;
        try {
            result.config.crs = crs.as<std::string>();
        } catch (const YAML::Exception&) {
            result.config.crs.clear();
        }
        if (result.config.crs.empty() || !crsFromUser(result.config.crs, error)) {
            result.error = path + ": crs: " + (error.empty() ? std::string("a text expected (EPSG:3857...)") : error);
            return result;
        }
    }

    // layers: [{source, style?, visible?, title?}, ...]
    const YAML::Node layers = root["layers"];
    if (layers && layers.IsSequence()) {
        for (std::size_t i = 0; i < layers.size(); ++i) {
            const YAML::Node item = layers[i];
            const std::string where = path + ": layers[" + std::to_string(i) + "]: ";
            if (!item.IsMap() || !item["source"]) {
                result.error = where + "expected 'source' (and optionally 'style')";
                return result;
            }
            unknownKeys(item, {"source", "style", "visible", "title", "crs"}, where, result.warnings);
            LayerSpec spec;
            try {
                spec.source = resolve(item["source"].as<std::string>(), folder);
                if (item["style"] && !item["style"].IsNull())
                    spec.style = resolve(item["style"].as<std::string>(), folder);
                if (item["visible"]) spec.visible = item["visible"].as<bool>();
                if (item["title"]) spec.title = item["title"].as<std::string>();
                if (item["crs"]) spec.crs = item["crs"].as<std::string>();   // §70: checked by the Map
            } catch (const YAML::Exception&) {
                result.error = where + "'visible' must be true or false";
                return result;
            }
            result.config.layers.push_back(spec);
        }
    }
    if (result.config.layers.empty()) {
        result.error = path + ": 'layers': at least one layer is needed";
        return result;
    }

    // start: {bbox: [xmin, ymin, xmax, ymax]}, optional.
    if (const YAML::Node start = root["start"]) {
        const YAML::Node bbox = start["bbox"];
        if (!bbox || !bbox.IsSequence() || bbox.size() != 4) {
            result.error = path + ": 'start: bbox: [xmin, ymin, xmax, ymax]' expected";
            return result;
        }
        try {
            const Envelope e{bbox[0].as<double>(), bbox[1].as<double>(), bbox[2].as<double>(), bbox[3].as<double>()};
            if (!e.isValid()) {
                result.error = path + ": start bbox: xmin < xmax and ymin < ymax expected";
                return result;
            }
            result.config.startBbox = e;
        } catch (const YAML::Exception&) {
            result.error = path + ": start bbox: four numbers expected";
            return result;
        }
    }

    result.ok = true;
    return result;
}

} // namespace r2d
