// SPDX-License-Identifier: AGPL-3.0-or-later
// render2d: the command. Reads vector layers with GDAL, draws them with a
// YSLD style (or a default one), writes PNG, SVG or PDF.
// Problems are reported next to the result instead of preventing it.
// See notions §21 and §26; since 0.3.5, a thin layer over r2d_map (§45).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/version.hpp"
#include "backend/cairo/cairo_target.hpp"
#include "backend/gdal/gdal_layer.hpp"
#include "core/view.hpp"
#include "map/config.hpp"
#include "map/map.hpp"
#include "render/labels.hpp"
#include "style/catalog.hpp"
#include "style/classify.hpp"
#include "style/ysld_reader.hpp"
#include "style/ysld_writer.hpp"

using namespace r2d;

namespace {

const char* kUsage =
    "usage: render2d SOURCE [SOURCE ...] [options]\n"
    "       render2d --config FILE.yaml [SOURCE ...] [options]\n"
    "\n"
    "  --config FILE.yaml     the layers (and start: bbox) of a viewer.yaml; relative paths\n"
    "                         from the file's folder. SOURCEs given too go on top;\n"
    "                         --bbox replaces start: bbox\n"
    "  SOURCE                 a file or connection string, vector or raster (GeoTIFF...);\n"
    "                         SOURCE::layer picks a layer\n"
    "                         (default: the first one). Drawn in order, first at the bottom.\n"
    "  --style FILE.yaml      YSLD style for the SOURCE just before (default: built-in style)\n"
    "  --bbox XMIN YMIN XMAX YMAX   area to draw (default: total extent of the layers)\n"
    "  --size W[xH]           image size (default: 1000 wide, height from the bbox shape)\n"
    "  --background RRGGBB|none     default FFFFFF; none = transparent\n"
    "  --pixel-ratio R        the image is seen R times smaller (1.5 on a screen scaled to\n"
    "                         150 %): style lengths times R, scale for the pixels seen (default 1)\n"
    "  -o FILE                output .png, .svg or .pdf (default: map.png)\n"
    "  --at X Y               what is at this point of the map (map units): the features the\n"
    "                         style draws there, top layer first, with their attributes.\n"
    "                         --bbox and --size set the scale; no image unless -o is given\n"
    "  --box XMIN YMIN XMAX YMAX    what is in this rectangle: the features the style draws\n"
    "                         whose geometry touches it (as --at: scale, -o, top layer first)\n"
    "  --area WKT             the same, in a polygon: --area \"POLYGON((x y, x y, x y, x y))\"\n"
    "  --where EXPRESSION     the features of the LAST SOURCE that make it true (§64), as the\n"
    "                         filters of the styles: --where \"AREA > 500 AND TYPE = 'maison'\".\n"
    "                         Every feature, whatever the scale. No image unless -o is given\n"
    "  --categorize FIELD     a style for the LAST SOURCE: one colour per value of FIELD (§63).\n"
    "                         Its --style gives the symbol (hatch, outline...), only the colour\n"
    "                         changes. Prints the legend; no image unless -o is given\n"
    "  --graduate FIELD       the same, in classes of a number, from light to dark\n"
    "  --classes N            --graduate: 2 to 10 classes (default 5)\n"
    "  --method equal|quantiles     --graduate: equal intervals (default) or as many features\n"
    "                         per class\n"
    "  --colours RRGGBB RRGGBB      --graduate: the light and the dark end (default FFF5EB 8C2D04)\n"
    "  --write-style FILE.yaml      writes the style made by --categorize or --graduate\n"
    "  --version              version of render2d, GDAL and Cairo\n";

struct SourceSpec {
    std::string                source;
    std::optional<std::string> style;   // YSLD file, from --style
};

struct Options {
    std::optional<std::string> config;   // --config: viewer.yaml (§57)
    std::vector<SourceSpec>  sources;
    std::optional<Envelope>  bbox;
    int                      width  = 1000;
    int                      height = 0;     // 0 = from the bbox shape
    std::optional<Color>     background = Color::fromHex(0xFFFFFF);
    std::string              output = "map.png";
    double                   pixelRatio = 1.0;   // notions §39
    std::optional<Point>     at;                 // --at: a query (§53)
    std::vector<Point>       zone;               // --box or --area: a zone (§56)
    std::string              zoneText;           // as given, for the answer's first line
    bool                     outputGiven = false;
    std::optional<ClassifySettings> classify;    // --categorize / --graduate (§63)
    std::string              writeStyle;         // --write-style
    std::string              where;              // --where (§64)
};

bool parseDouble(const char* s, double& out) {
    char* end = nullptr;
    out = std::strtod(s, &end);
    return end != s && *end == '\0';
}

// Returns false on a usage error (message already printed).
bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](int n) {
            if (i + n >= argc) { std::fprintf(stderr, "%s needs %d value(s)\n", a.c_str(), n); return false; }
            return true;
        };
        if (a == "--version") {
            std::printf("render2d %s (%s), built %s\nGDAL %s, Cairo %s\n", kVersion, kVersionStep, __DATE__,
                        gdalVersion().c_str(), cairoVersion().c_str());
            for (const std::string& line : gdalDataInfo())   // §68: where the reference files come from
                std::printf("%s\n", line.c_str());
            std::exit(0);
        } else if (a == "-h" || a == "--help") {
            std::fputs(kUsage, stdout);
            std::exit(0);
        } else if (a == "--config") {
            if (!need(1)) return false;
            o.config = argv[++i];
        } else if (a == "--bbox") {
            if (!need(4)) return false;
            double v[4];
            for (int k = 0; k < 4; ++k) {
                if (!parseDouble(argv[i + 1 + k], v[k])) {
                    std::fprintf(stderr, "--bbox: '%s' is not a number\n", argv[i + 1 + k]);
                    return false;
                }
            }
            o.bbox = Envelope{v[0], v[1], v[2], v[3]};
            i += 4;
        } else if (a == "--size") {
            if (!need(1)) return false;
            const std::string s = argv[++i];
            const auto x = s.find('x');
            o.width  = std::atoi(s.substr(0, x).c_str());
            o.height = x == std::string::npos ? 0 : std::atoi(s.substr(x + 1).c_str());
            if (o.width <= 0 || o.height < 0) {
                std::fprintf(stderr, "--size: expected W or WxH, got '%s'\n", s.c_str());
                return false;
            }
        } else if (a == "--background") {
            if (!need(1)) return false;
            const std::string s = argv[++i];
            if (s == "none") o.background.reset();
            else o.background = Color::fromHex(static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 16)));
        } else if (a == "--pixel-ratio") {
            if (!need(1)) return false;
            if (!parseDouble(argv[++i], o.pixelRatio) || !(o.pixelRatio > 0.0 && o.pixelRatio <= 8.0)) {
                std::fprintf(stderr, "--pixel-ratio: expected a number between 0 and 8, got '%s'\n", argv[i]);
                return false;
            }
        } else if (a == "--style") {
            if (!need(1)) return false;
            if (o.sources.empty()) {
                std::fprintf(stderr, "--style must follow the SOURCE it applies to\n");
                return false;
            }
            o.sources.back().style = argv[++i];
        } else if (a == "-o") {
            if (!need(1)) return false;
            o.output = argv[++i];
            o.outputGiven = true;
        } else if (a == "--at") {
            if (!need(2)) return false;
            double x = 0, y = 0;
            if (!parseDouble(argv[i + 1], x) || !parseDouble(argv[i + 2], y)) {
                std::fprintf(stderr, "--at: expected two numbers, got '%s %s'\n", argv[i + 1], argv[i + 2]);
                return false;
            }
            o.at = Point{x, y};
            i += 2;
        } else if (a == "--box") {
            if (!need(4)) return false;
            double v[4];
            for (int k = 0; k < 4; ++k) {
                if (!parseDouble(argv[i + 1 + k], v[k])) {
                    std::fprintf(stderr, "--box: '%s' is not a number\n", argv[i + 1 + k]);
                    return false;
                }
            }
            const double x0 = std::min(v[0], v[2]), x1 = std::max(v[0], v[2]);
            const double y0 = std::min(v[1], v[3]), y1 = std::max(v[1], v[3]);
            o.zone     = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
            char text[160];
            std::snprintf(text, sizeof text, "box %.2f %.2f %.2f %.2f", x0, y0, x1, y1);
            o.zoneText = text;
            i += 4;
        } else if (a == "--area") {
            if (!need(1)) return false;
            std::string error;
            if (!zoneFromWkt(argv[++i], o.zone, error)) {
                std::fprintf(stderr, "--area: %s\n", error.c_str());
                return false;
            }
            o.zoneText = "area of " + std::to_string(o.zone.size()) + " points";
        } else if (a == "--where") {
            if (!need(1)) return false;
            o.where = argv[++i];
        } else if (a == "--categorize" || a == "--graduate") {
            if (!need(1)) return false;
            if (!o.classify) o.classify = ClassifySettings{};
            o.classify->graduate = a == "--graduate";
            o.classify->field    = argv[++i];
        } else if (a == "--classes") {
            if (!need(1)) return false;
            if (!o.classify) o.classify = ClassifySettings{};
            o.classify->classes = std::atoi(argv[++i]);
            if (o.classify->classes < 2 || o.classify->classes > 10) {
                std::fprintf(stderr, "--classes: expected 2 to 10, got '%s'\n", argv[i]);
                return false;
            }
        } else if (a == "--method") {
            if (!need(1)) return false;
            if (!o.classify) o.classify = ClassifySettings{};
            const std::string m = argv[++i];
            if (m == "equal") o.classify->method = BreakMethod::EqualIntervals;
            else if (m == "quantiles") o.classify->method = BreakMethod::Quantiles;
            else {
                std::fprintf(stderr, "--method: expected equal or quantiles, got '%s'\n", m.c_str());
                return false;
            }
        } else if (a == "--colours" || a == "--colors") {
            if (!need(2)) return false;
            if (!o.classify) o.classify = ClassifySettings{};
            o.classify->from = Color::fromHex(static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 16)));
            o.classify->to   = Color::fromHex(static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 16)));
        } else if (a == "--write-style") {
            if (!need(1)) return false;
            o.writeStyle = argv[++i];
        } else if (!a.empty() && a[0] == '-' && a.rfind("PG:", 0) != 0) {
            std::fprintf(stderr, "unknown option '%s'\n", a.c_str());
            return false;
        } else {
            o.sources.push_back({a, std::nullopt});
        }
    }
    if (o.sources.empty() && !o.config) {
        std::fputs(kUsage, stderr);
        return false;
    }
    if (o.classify && (o.classify->field.empty() || o.sources.empty())) {
        std::fprintf(stderr, "--classes, --method, --colours need --categorize or --graduate FIELD, "
                             "and a SOURCE on the command line\n");
        return false;
    }
    if (!o.where.empty() && o.sources.empty()) {
        std::fprintf(stderr, "--where needs a SOURCE on the command line\n");
        return false;
    }
    if (!o.writeStyle.empty() && !o.classify) {
        std::fprintf(stderr, "--write-style goes with --categorize or --graduate\n");
        return false;
    }
    return true;
}

// §63: the style for the last SOURCE, made by classify() as editor2d's
// assistant does. Prints the legend; writes the style if asked. The YSLD text,
// or "" when it could not be made (said on stderr).
std::string classifyLastSource(const Options& opt, int index) {
    const SourceSpec& src = opt.sources.back();
    const ClassifySettings& set = *opt.classify;
    std::string file = src.source, layerName, error;
    if (const auto sep = src.source.rfind("::"); sep != std::string::npos) {
        file      = src.source.substr(0, sep);
        layerName = src.source.substr(sep + 2);
    }
    auto layer = GdalLayer::open(file, layerName, error);
    if (!layer) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return {};
    }
    // The symbol to keep: the --style of that SOURCE, else the default look.
    Style base;
    if (src.style) {
        const YsldResult r = readYsldFile(*src.style);
        if (!r.style) {
            std::fprintf(stderr, "error: %s\n", r.error.c_str());
            return {};
        }
        base = *r.style;
    }
    const auto type = layer->info().type;
    const SymbolizerKind kind = !type || *type == GeomType::Polygon ? SymbolizerKind::Polygon
                              : *type == GeomType::Line             ? SymbolizerKind::Line
                                                                    : SymbolizerKind::Point;
    const auto t0 = std::chrono::steady_clock::now();
    const FieldSummary summary = summarizeField(*layer, set.field, set.maxDistinct);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const Classified c = classify(base, newSymbolizer(kind, index), summary, set);
    if (!c.ok) {
        std::fprintf(stderr, "error: %s\n", c.error.c_str());
        return {};
    }

    // The legend, as editor2d's preview shows it.
    std::printf("%s %s: %d features, %d without value (read in %.0f ms)\n",
                set.graduate ? "graduate" : "categorize", set.field.c_str(), summary.total, summary.nulls, ms);
    for (const LegendEntry& e : c.legend) {
        auto byte = [](double v) { return static_cast<int>(std::lround(v * 255.0)); };
        std::printf("  #%02X%02X%02X  %8d  %s\n", byte(e.colour.r), byte(e.colour.g), byte(e.colour.b), e.count,
                    e.name.c_str());
    }
    if (!opt.writeStyle.empty()) {
        const std::filesystem::path out(opt.writeStyle);
        std::ofstream f(out, std::ios::binary);
        f << writeYsld(c.style, out.parent_path().string());
        if (!f) {
            std::fprintf(stderr, "error: cannot write %s\n", opt.writeStyle.c_str());
            return {};
        }
        std::printf("wrote %s\n", opt.writeStyle.c_str());
    }
    return writeYsld(c.style);
}

// A value as the report shows it: NULL, 120, 412.6, "a text".
std::string valueText(const Value& v) {
    if (isNull(v)) return "NULL";
    if (const auto* t = std::get_if<std::string>(&v)) return "\"" + *t + "\"";
    return labelText(v);
}

// One feature found: "  layer  fid F  FIELD=value ...".
void printHit(Map& map, const QueryHit& h) {
    const LayerInfo& li = map.layerInfo(h.layer);
    std::string line = "  " + li.name + "  fid " + std::to_string(h.fid);
    const auto& fields = map.layerFields(h.layer);
    for (std::size_t c = 0; c < fields.size() && c < h.attributes.size(); ++c)
        line += "  " + fields[c].name + "=" + valueText(h.attributes[c]);
    std::printf("%s\n", line.c_str());
}

// §53: the answer to --at, top layer first.
void printQuery(Map& map, const View& view, Point at) {
    constexpr double kTolerancePx = 4.0;
    const Point pixel = view.worldToImage.apply(at);
    const auto hits   = map.queryAt(view, pixel, kTolerancePx);
    const double scale = view.resolution * map.pixelRatio() / kStandardPixelSizeM;
    std::printf("at %.2f %.2f (scale 1:%.0f, tolerance %.0f px = %.2f m): %zu hit%s\n", at.x, at.y, scale,
                kTolerancePx, kTolerancePx * view.resolution, hits.size(), hits.size() == 1 ? "" : "s");
    for (const QueryHit& h : hits) printHit(map, h);
}

// §64: the answer to --where, on the last layer.
bool printWhereQuery(Map& map, const std::string& where) {
    const auto t0 = std::chrono::steady_clock::now();
    const AreaQuery q = map.queryWhere(map.layerCount() - 1, where);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!q.error.empty()) {
        std::printf("where %s: error: %s\n", where.c_str(), q.error.c_str());
        return false;
    }
    std::printf("where %s: %zu hit%s (%.0f ms)\n", where.c_str(), q.matched, q.matched == 1 ? "" : "s", ms);
    for (const QueryHit& h : q.hits) printHit(map, h);
    return true;
}

// §56: the answer to --box / --area, top layer first.
void printZoneQuery(Map& map, const View& view, const std::vector<Point>& zone, const std::string& text) {
    const AreaQuery q   = map.queryArea(view, zone);
    const double scale  = view.resolution * map.pixelRatio() / kStandardPixelSizeM;
    if (!q.error.empty()) {
        std::printf("in %s: error: %s\n", text.c_str(), q.error.c_str());
        return;
    }
    std::printf("in %s (scale 1:%.0f): %zu hit%s\n", text.c_str(), scale, q.hits.size(),
                q.hits.size() == 1 ? "" : "s");
    for (const QueryHit& h : q.hits) printHit(map, h);
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 2;

    std::vector<std::string> warnings;
    auto warn = [&](const std::string& w) { warnings.push_back(w); };

    // 0. A config file (§57): its layers first, those of the command line on top.
    std::vector<LayerSpec>   specs;
    std::optional<Envelope>  startBbox;
    std::vector<std::string> configLines;   // printed at the top of the report
    if (opt.config) {
        const ConfigResult c = loadConfig(*opt.config);
        if (!c.ok) {
            std::fprintf(stderr, "error: %s\n", c.error.c_str());
            return 1;
        }
        for (const auto& w : c.warnings) warn(w);
        specs     = c.config.layers;
        startBbox = c.config.startBbox;
        configLines.push_back("config: " + std::filesystem::absolute(c.config.path).lexically_normal().generic_string());
    }
    for (const SourceSpec& s : opt.sources) specs.push_back({s.source, s.style});
    // §57: the report says what was really read, paths resolved.
    if (opt.config) {
        for (std::size_t i = 0; i < specs.size(); ++i) {
            std::string line = "  layer " + std::to_string(i + 1) + ": " + specs[i].source;
            if (!specs[i].visible) line += "  (hidden)";
            configLines.push_back(line);
            configLines.push_back("           style " + (specs[i].style ? *specs[i].style : std::string("(default)")));
        }
    }

    // 1. The map: layers opened, styles read and scaled, CRS checked (§46).
    //    Problems are warnings; only "no layer at all" stops here.
    Map map = Map::open(specs, opt.pixelRatio);
    for (const auto& w : map.warnings()) warn(w);
    if (map.empty()) {
        for (const auto& w : warnings) std::fprintf(stderr, "error: %s\n", w.c_str());
        std::fprintf(stderr, "nothing to draw\n");
        return 1;
    }

    // 2. Area: given, or the total extent of the layers.
    // --bbox, else the config's start bbox, else everything.
    std::optional<Envelope> area = opt.bbox ? opt.bbox : startBbox ? startBbox : map.extent();
    if (!area || !area->isValid()) {
        std::fprintf(stderr, "error: no valid area (give --bbox)\n");
        return 1;
    }

    // 3. Image size: height from the area shape when not given.
    ImageSize size{opt.width, opt.height};
    if (size.height == 0) {
        size.height = std::max(1, static_cast<int>(std::lround(opt.width * area->height() / area->width())));
    }
    auto view = makeView(*area, size);
    if (!view) {
        std::fprintf(stderr, "error: invalid view\n");
        return 1;
    }
    // Only meaningful when the user imposed both W and H. When the height is
    // computed, rounding it to whole pixels always leaves a sub-pixel
    // difference: not worth a warning.
    if (view->widened && opt.height != 0) {
        warn("area widened to keep square pixels (aspect ratio of --size)");
    }

    // §63: --categorize / --graduate: a new style for the last SOURCE.
    if (opt.classify) {
        std::printf("render2d %s\n", kVersion);
        const std::string ysld = classifyLastSource(opt, static_cast<int>(specs.size()) - 1);
        if (ysld.empty()) return 1;
        if (!opt.outputGiven) return 0;   // the legend (and the file) only
        const int last = map.layerCount() - 1;
        if (map.layerCount() != static_cast<int>(specs.size())) {
            std::fprintf(stderr, "error: a layer could not be opened: the classified one cannot be told\n");
            return 1;
        }
        std::vector<std::string> said;
        if (!map.setLayerStyle(last, ysld, said)) {
            for (const auto& w : said) std::fprintf(stderr, "error: %s\n", w.c_str());
            return 1;
        }
        for (const auto& w : said) warn(w);
    }

    // §64: --where reads the last SOURCE given on the command line.
    if (!opt.where.empty() && map.layerCount() != static_cast<int>(specs.size())) {
        std::fprintf(stderr, "error: a layer could not be opened: the one for --where cannot be told\n");
        return 1;
    }

    // --at, --box, --area or --where without -o: only the question, no image (§53, §56, §64).
    const bool asked = opt.at || !opt.zone.empty() || !opt.where.empty();
    if (asked && !opt.outputGiven) {
        if (!opt.classify) std::printf("render2d %s\n", kVersion);   // §63: already said
        for (const auto& line : configLines) std::printf("%s\n", line.c_str());
        if (opt.at) printQuery(map, *view, *opt.at);
        if (!opt.zone.empty()) printZoneQuery(map, *view, opt.zone, opt.zoneText);
        if (!opt.where.empty() && !printWhereQuery(map, opt.where)) return 1;
        for (const auto& w : warnings) std::printf("warning: %s\n", w.c_str());
        return 0;
    }

    // 4. Output surface.
    std::string error;
    auto target = CairoTarget::create(opt.output, view->size, error);
    if (!target) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    Canvas& cv = target->canvas();
    cv.clear(opt.background ? *opt.background : Color{0.0, 0.0, 0.0, 0.0});

    // 5. Draw, then the report (§47): the same lines as editor2d shows.
    const MapReport report = map.render(cv, *view);
    if (!opt.classify) std::printf("render2d %s\n", kVersion);   // §63: already said
    for (const auto& line : configLines) std::printf("%s\n", line.c_str());
    for (const auto& line : reportLines(report)) std::printf("%s\n", line.c_str());
    for (const auto& w : report.warnings) warn(w);

    // 6. Write the file (PNG compression or SVG/PDF completion), then the warnings.
    using Clock = std::chrono::steady_clock;
    const auto w0 = Clock::now();
    if (!target->finish(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    const double msWrite = std::chrono::duration<double, std::milli>(Clock::now() - w0).count();
    std::printf("wrote %s (%.1f ms)\n", opt.output.c_str(), msWrite);
    if (opt.at) printQuery(map, *view, *opt.at);
    if (!opt.zone.empty()) printZoneQuery(map, *view, opt.zone, opt.zoneText);
    if (!opt.where.empty() && !printWhereQuery(map, opt.where)) return 1;
    for (const auto& w : warnings) std::printf("warning: %s\n", w.c_str());
    return 0;
}
