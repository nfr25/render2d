// SPDX-License-Identifier: AGPL-3.0-or-later
#include "style/ysld_reader.hpp"

#include <cctype>
#include <filesystem>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include <yaml-cpp/yaml.h>

namespace r2d {

namespace {

// Walks the YAML tree, builds the Style, collects warnings.
class Reader {
public:
    Reader(std::vector<std::string>& warnings, std::string baseDir)
        : warnings_(warnings), baseDir_(std::move(baseDir)) {}

    Style readRoot(const YAML::Node& root);

private:
    std::vector<std::string>& warnings_;
    std::string               baseDir_;   // the style file's folder: images are relative to it

    // --- messages ------------------------------------------------------
    void warn(const YAML::Node& at, const std::string& msg) {
        const int line = at.Mark().line;
        warnings_.push_back(line >= 0 ? "line " + std::to_string(line + 1) + ": " + msg : msg);
    }
    void unknownKey(const YAML::Node& key, const std::string& where) {
        warn(key, "unknown or unsupported key '" + key.as<std::string>() + "' in " + where + " (ignored)");
    }

    // --- values --------------------------------------------------------
    bool readNumber(const YAML::Node& key, const YAML::Node& v, double& out) {
        try {
            out = v.as<double>();
            return true;
        } catch (const YAML::Exception&) {
            warn(key, "'" + key.as<std::string>() + "' expects a number (value ignored)");
            return false;
        }
    }

    // '#RRGGBB' or '#RGB'. An unquoted #... is a YAML comment: the value is
    // then empty, which deserves a precise hint. notions §23.
    std::optional<Color> readColor(const YAML::Node& key, const YAML::Node& v) {
        const std::string name = key.as<std::string>();
        if (v.IsNull()) {
            warn(key, "'" + name + "' has no value: a colour must be quoted, '#RRGGBB', "
                      "otherwise YAML reads # as the start of a comment");
            return std::nullopt;
        }
        if (!v.IsScalar()) {
            warn(key, "'" + name + "' expects a colour '#RRGGBB'");
            return std::nullopt;
        }
        std::string s = v.as<std::string>();
        if (!s.empty() && s[0] == '#') s = s.substr(1);
        if (s.size() == 3) s = {s[0], s[0], s[1], s[1], s[2], s[2]};
        bool hex = s.size() == 6;
        for (char ch : s) hex = hex && std::isxdigit(static_cast<unsigned char>(ch));
        if (!hex) {
            warn(key, "'" + name + "': '" + v.as<std::string>() + "' is not a colour '#RRGGBB'");
            return std::nullopt;
        }
        return Color::fromHex(static_cast<std::uint32_t>(std::stoul(s, nullptr, 16)));
    }

    // "8 4" or [8, 4], in pixels.
    std::vector<double> readDashes(const YAML::Node& key, const YAML::Node& v) {
        std::vector<double> d;
        try {
            if (v.IsSequence()) {
                for (const auto& x : v) d.push_back(x.as<double>());
            } else {
                std::istringstream in(v.as<std::string>());
                double x;
                while (in >> x) d.push_back(x);
                if (!in.eof()) d.clear();
            }
        } catch (const YAML::Exception&) {
            d.clear();
        }
        if (d.empty()) warn(key, "'stroke-dasharray' expects lengths like '8 4' (ignored)");
        return d;
    }

    // [min, 15000], [15000, max]: notions §24.
    ScaleRange readScale(const YAML::Node& key, const YAML::Node& v) {
        ScaleRange r;
        if (!v.IsSequence() || v.size() != 2) {
            warn(key, "'scale' expects [min, max], e.g. [min, 15000] (ignored)");
            return r;
        }
        auto bound = [&](const YAML::Node& b, const char* word, double& out) {
            if (b.IsScalar() && b.as<std::string>() == word) return;   // no limit
            double x;
            if (readNumber(key, b, x)) out = x;
        };
        bound(v[0], "min", r.min);
        bound(v[1], "max", r.max);
        if (!(r.min < r.max)) {
            warn(key, "'scale': the first bound must be smaller than the second (ignored)");
            return ScaleRange{};
        }
        return r;
    }

    // --- strokes and fills shared by the symbolizers -------------------
    struct StrokeKeys {
        std::optional<Color> color;
        double               opacity = 1.0;
        StrokeStyle          style;          // width, cap, join, dashes
        bool                 otherKeys = false;
    };

    // True when the key was a stroke-* key (handled or reported).
    // `where` names the enclosing symbolizer in messages.
    bool readStrokeKey(const YAML::Node& key, const YAML::Node& v, StrokeKeys& s, const char* where) {
        const std::string k = key.as<std::string>();
        if (k.rfind("stroke-", 0) != 0) return false;
        if (k == "stroke-color") {
            s.color = readColor(key, v);
            return true;
        }
        bool known = true;
        if (k == "stroke-width") {
            double x;
            if (readNumber(key, v, x)) s.style.width = x;
        } else if (k == "stroke-opacity") {
            double x;
            if (readNumber(key, v, x)) s.opacity = x;
        } else if (k == "stroke-linecap") {
            const std::string c = v.IsScalar() ? v.as<std::string>() : "";
            if      (c == "butt")   s.style.cap = LineCap::Butt;
            else if (c == "round")  s.style.cap = LineCap::Round;
            else if (c == "square") s.style.cap = LineCap::Square;
            else warn(key, "'stroke-linecap' expects butt, round or square (ignored)");
        } else if (k == "stroke-linejoin") {
            const std::string j = v.IsScalar() ? v.as<std::string>() : "";
            if      (j == "miter" || j == "mitre") s.style.join = LineJoin::Miter;
            else if (j == "round")                 s.style.join = LineJoin::Round;
            else if (j == "bevel")                 s.style.join = LineJoin::Bevel;
            else warn(key, "'stroke-linejoin' expects miter, round or bevel (ignored)");
        } else if (k == "stroke-dasharray") {
            s.style.dashes = readDashes(key, v);
        } else if (k == "stroke-dashoffset") {
            double x;
            if (readNumber(key, v, x)) s.style.dashOffset = x;
        } else {
            known = false;
            unknownKey(key, where);
        }
        if (known) s.otherKeys = true;   // a stroke setting, even without a colour
        return true;
    }

    static StrokeStyle finishStroke(const StrokeKeys& s, Color fallback) {
        StrokeStyle out = s.style;
        out.color   = s.color ? *s.color : fallback;
        out.color.a = s.opacity;
        return out;
    }

    // --- symbolizers -----------------------------------------------------
    PolygonSymbolizer readPolygon(const YAML::Node& node) {
        PolygonSymbolizer p;
        StrokeKeys           stroke;
        std::optional<Color> fillColor;
        double               fillOpacity = 1.0;
        for (const auto& kv : node) {
            const std::string k = kv.first.as<std::string>();
            if (readStrokeKey(kv.first, kv.second, stroke, "polygon")) continue;
            if (k == "fill-color") {
                fillColor = readColor(kv.first, kv.second);
            } else if (k == "fill-graphic") {
                p.graphicFill = readGraphicFill(kv.first, kv.second);
            } else if (k == "fill-opacity") {
                readNumber(kv.first, kv.second, fillOpacity);
            } else {
                unknownKey(kv.first, "polygon");
            }
        }
        if (fillColor) {
            Color c = *fillColor;
            c.a = fillOpacity;
            p.fill = FillStyle{.color = c};   // even-odd by default, notions §11
        }
        if (stroke.color) {
            p.stroke = finishStroke(stroke, Color{});
        } else if (stroke.otherKeys) {
            warn(node, "polygon has stroke settings but no 'stroke-color': no outline is drawn");
        }
        if (!p.fill && !p.stroke && !p.graphicFill)
            warn(node, "polygon has neither 'fill-color', 'fill-graphic' nor 'stroke-color': it draws nothing");
        return p;
    }

    LineSymbolizer readLine(const YAML::Node& node) {
        StrokeKeys stroke;
        for (const auto& kv : node) {
            if (!readStrokeKey(kv.first, kv.second, stroke, "line")) unknownKey(kv.first, "line");
        }
        return LineSymbolizer{finishStroke(stroke, Color::fromHex(0x000000))};  // SLD default: black
    }

    // shape: circle, square, triangle, star, cross, x, shape://slash... §40, §43.
    MarkShape readShape(const YAML::Node& key, const YAML::Node& v) {
        static const std::pair<const char*, MarkShape> names[] = {
            {"circle", MarkShape::Circle}, {"square", MarkShape::Square}, {"triangle", MarkShape::Triangle},
            {"star", MarkShape::Star}, {"cross", MarkShape::Cross}, {"x", MarkShape::X},
            {"shape://slash", MarkShape::Slash}, {"shape://backslash", MarkShape::Backslash},
            {"shape://horline", MarkShape::HorLine}, {"shape://vertline", MarkShape::VertLine},
            {"shape://times", MarkShape::Times}, {"shape://dot", MarkShape::Dot}};
        const std::string name = v.IsScalar() ? v.as<std::string>() : "";
        for (const auto& [n, shape] : names) if (name == n) return shape;
        warn(key, "unknown shape '" + name + "' (circle, square, triangle, star, cross, x, "
                  "shape://slash, backslash, horline, vertline, times, dot): drawn as a circle");
        return MarkShape::Circle;
    }

    // mark: {shape, fill-color, fill-opacity, stroke-...}, shared by points and fills.
    void readMark(const YAML::Node& node, MarkShape& shape, std::optional<FillStyle>& fill,
                  std::optional<StrokeStyle>& strokeOut, bool fillByDefault) {
        // SLD defaults for a mark: grey fill, black 1-pixel outline.
        std::optional<Color> fillColor;
        if (fillByDefault) fillColor = Color::fromHex(0x808080);
        double     fillOpacity = 1.0;
        StrokeKeys stroke;
        stroke.color = Color::fromHex(0x000000);
        for (const auto& m : node) {
            const std::string mk = m.first.as<std::string>();
            if (readStrokeKey(m.first, m.second, stroke, "mark")) continue;
            if      (mk == "shape")        shape = readShape(m.first, m.second);
            else if (mk == "fill-color")   fillColor = readColor(m.first, m.second);
            else if (mk == "fill-opacity") readNumber(m.first, m.second, fillOpacity);
            else unknownKey(m.first, "mark");
        }
        if (fillColor) {
            Color c = *fillColor;
            c.a = fillOpacity;
            fill = FillStyle{.color = c};
        }
        if (stroke.color) strokeOut = finishStroke(stroke, Color{});
    }

    // An image path, relative to the style file. §42.
    std::string resolve(const std::string& url) const {
        std::filesystem::path p(url);
        if (p.is_relative() && !baseDir_.empty()) p = std::filesystem::path(baseDir_) / p;
        return p.lexically_normal().string();
    }

    // point: {size, rotation, uom, symbols: [{external: {url, format}}, {mark: {...}}]}. §40 to §42.
    // SLD: the first symbol that can be drawn. An image comes first, the mark is its fallback.
    PointSymbolizer readPoint(const YAML::Node& node) {
        PointSymbolizer pt;
        bool haveMark = false, haveSize = false;
        pt.fill = FillStyle{.color = Color::fromHex(0x808080)};
        pt.stroke = StrokeStyle{.color = Color::fromHex(0x000000)};

        for (const auto& kv : node) {
            const std::string k = kv.first.as<std::string>();
            const YAML::Node& v = kv.second;
            if (k == "size") {
                haveSize = true;
                const std::string t = v.IsScalar() ? v.as<std::string>() : "";
                if (t.rfind("${", 0) == 0) pt.sizeExpr = readExpression(kv.first, v, false);
                else readNumber(kv.first, v, pt.size);
            } else if (k == "rotation") {
                pt.rotation = readExpression(kv.first, v, false);
            } else if (k == "uom") {
                const std::string u = v.as<std::string>("");
                if (u == "metre" || u == "meter" || u.find("/metre") != std::string::npos) pt.metres = true;
                else if (u != "pixel" && u.find("/pixel") == std::string::npos)
                    warn(kv.first, "'uom' expects metre or pixel (pixel kept)");
            } else if (k == "symbols") {
                if (!v.IsSequence()) { warn(kv.first, "'symbols' expects a list"); continue; }
                for (const auto& sym : v) {
                    if (!sym.IsMap()) { warn(sym, "a symbol is a mark or an external image"); continue; }
                    if (sym["external"] || sym["external-graphic"]) {
                        const YAML::Node e = sym["external"] ? sym["external"] : sym["external-graphic"];
                        if (haveMark || !pt.image.empty()) { warn(sym, "an image after the first symbol is never used (ignored)"); continue; }
                        const std::string url = e["url"] ? e["url"].as<std::string>("") : "";
                        const std::string format = e["format"] ? e["format"].as<std::string>("image/png") : "image/png";
                        if (format != "image/png") {
                            warn(sym, "only PNG images (image/png) are supported yet: '" + format + "' ignored");
                        } else if (url.empty()) {
                            warn(sym, "'external' needs a 'url'");
                        } else if (!std::filesystem::exists(resolve(url))) {
                            warn(sym, "image not found: " + resolve(url) + " (the next symbol is drawn instead)");
                        } else {
                            pt.image = resolve(url);
                        }
                    } else if (sym["mark"]) {
                        if (haveMark) { warn(sym, "only the first mark is drawn (others ignored)"); continue; }
                        haveMark = true;
                        pt.fill.reset();
                        pt.stroke.reset();
                        readMark(sym["mark"], pt.shape, pt.fill, pt.stroke, true);
                    } else {
                        warn(sym, "a symbol is 'mark' or 'external' (ignored)");
                    }
                }
            } else {
                unknownKey(kv.first, "point");
            }
        }
        if (!pt.image.empty() && !haveSize) pt.size = 0.0;   // an image keeps its own size
        return pt;
    }

    // fill-graphic: {size: 8, symbols: [{mark: {shape: 'shape://slash', stroke-color, stroke-width}}]}. §43.
    GraphicFill readGraphicFill(const YAML::Node& key, const YAML::Node& node) {
        GraphicFill gf;
        if (!node.IsMap()) { warn(key, "'fill-graphic' expects {size, symbols}"); return gf; }
        bool haveMark = false;
        for (const auto& kv : node) {
            const std::string k = kv.first.as<std::string>();
            if (k == "size") {
                readNumber(kv.first, kv.second, gf.size);
            } else if (k == "symbols" && kv.second.IsSequence()) {
                for (const auto& sym : kv.second) {
                    if (!sym.IsMap() || !sym["mark"]) { warn(sym, "a fill-graphic takes a 'mark' (images not yet)"); continue; }
                    if (haveMark) { warn(sym, "only the first mark is used (others ignored)"); continue; }
                    haveMark = true;
                    readMark(sym["mark"], gf.shape, gf.fill, gf.stroke, false);
                }
            } else {
                unknownKey(kv.first, "fill-graphic");
            }
        }
        if (!haveMark) warn(key, "'fill-graphic' has no mark: diagonal lines are drawn");
        if (!gf.stroke && !gf.fill) gf.stroke = StrokeStyle{.color = Color::fromHex(0x000000)};
        return gf;
    }

    // label: ${POLICENUM} (an expression) or Evere (a fixed text, when `plain`);
    // rotation: ${360 - CARTOANGLE} or 45. notions §35, §36.
    std::optional<Expression> readExpression(const YAML::Node& key, const YAML::Node& v, bool plain) {
        const std::string name = key.as<std::string>();
        if (!v.IsScalar()) {
            warn(key, "'" + name + "' expects a value or an expression like ${FIELD}; "
                      "inside [ ] or { }, put it between quotes");
            return std::nullopt;
        }
        std::string text = v.as<std::string>();
        const bool braces = text.size() >= 3 && text.compare(0, 2, "${") == 0 && text.back() == '}';
        if (braces) text = text.substr(2, text.size() - 3);
        else if (plain) return Expression::literalText(text);
        std::string error;
        auto e = Expression::parseValue(text, error);
        if (!e) warn(key, "'" + name + "' " + text + ": " + error + " (ignored)");
        return e;
    }

    // [x, y], two numbers.
    bool readPair(const YAML::Node& key, const YAML::Node& v, double& x, double& y) {
        try {
            if (v.IsSequence() && v.size() == 2) { x = v[0].as<double>(); y = v[1].as<double>(); return true; }
        } catch (const YAML::Exception&) {}
        warn(key, "'" + key.as<std::string>() + "' expects two numbers, e.g. [0.5, 0.5] (ignored)");
        return false;
    }

    // text: {label: ${POLICENUM}, font-family, font-size, fill-color, halo: {radius, fill-color},
    //        anchor: [0.5, 0.5], displacement: [0, 0], rotation: ${360 - CARTOANGLE}}. §35.
    TextSymbolizer readText(const YAML::Node& node) {
        TextSymbolizer t;
        double fillOpacity = 1.0;
        for (const auto& kv : node) {
            const std::string k = kv.first.as<std::string>();
            const YAML::Node& v = kv.second;
            if      (k == "label")        t.label = readExpression(kv.first, v, true);
            else if (k == "rotation")     t.rotation = readExpression(kv.first, v, false);
            else if (k == "font-family")  t.font.family = v.as<std::string>(t.font.family);
            else if (k == "font-size")    readNumber(kv.first, v, t.font.size);
            else if (k == "font-weight")  t.font.bold = v.as<std::string>("") == "bold";
            else if (k == "font-style") {
                const std::string st = v.as<std::string>("");
                t.font.italic = st == "italic" || st == "oblique";
            }
            else if (k == "fill-color")   { if (auto c = readColor(kv.first, v)) t.fill = *c; }
            else if (k == "fill-opacity") readNumber(kv.first, v, fillOpacity);
            else if (k == "anchor")       readPair(kv.first, v, t.anchorX, t.anchorY);
            else if (k == "displacement") readPair(kv.first, v, t.displacementX, t.displacementY);
            else if (k == "placement") {
                if (v.as<std::string>("") != "point")
                    warn(kv.first, "only 'placement: point' is supported yet: drawn at a point");
            }
            else if (k == "halo") {
                if (!v.IsMap()) { warn(kv.first, "'halo' expects {radius, fill-color}"); continue; }
                Halo h;
                double haloOpacity = 1.0;
                for (const auto& hv : v) {
                    const std::string hk = hv.first.as<std::string>();
                    if      (hk == "radius")       readNumber(hv.first, hv.second, h.radius);
                    else if (hk == "fill-color")   { if (auto c = readColor(hv.first, hv.second)) h.color = *c; }
                    else if (hk == "fill-opacity") readNumber(hv.first, hv.second, haloOpacity);
                    else unknownKey(hv.first, "halo");
                }
                h.color.a = haloOpacity;
                t.halo = h;
            }
            else unknownKey(kv.first, "text");
        }
        t.fill.a = fillOpacity;
        if (!t.label) warn(node, "text has no 'label': nothing will be written");
        return t;
    }

    // raster: {opacity}. §66.
    RasterSymbolizer readRaster(const YAML::Node& node) {
        RasterSymbolizer r;
        for (const auto& kv : node) {
            const std::string k = kv.first.as<std::string>();
            if (k == "opacity") {
                readNumber(kv.first, kv.second, r.opacity);
                if (!(r.opacity >= 0.0 && r.opacity <= 1.0)) {
                    warn(kv.first, "'opacity' goes from 0 to 1: 1 used");
                    r.opacity = 1.0;
                }
            }
            else unknownKey(kv.first, "raster");
        }
        return r;
    }

    void readSymbolizers(const YAML::Node& key, const YAML::Node& list, Rule& rule) {
        if (!list.IsSequence()) { warn(key, "'symbolizers' expects a list"); return; }
        for (const auto& item : list) {
            if (!item.IsMap() || item.size() != 1) {
                warn(item, "each symbolizer is one of: polygon, line, point, text, raster");
                continue;
            }
            const auto kv = *item.begin();
            const std::string type = kv.first.as<std::string>();
            const YAML::Node& body = kv.second;
            if (!body.IsMap() && !body.IsNull()) { warn(kv.first, "'" + type + "' expects settings"); continue; }
            if      (type == "polygon") rule.symbolizers.emplace_back(readPolygon(body));
            else if (type == "line")    rule.symbolizers.emplace_back(readLine(body));
            else if (type == "point")   rule.symbolizers.emplace_back(readPoint(body));
            else if (type == "text")    rule.symbolizers.emplace_back(readText(body));
            else if (type == "raster")  rule.symbolizers.emplace_back(readRaster(body));
            else warn(kv.first, "symbolizer '" + type + "' is not supported yet (ignored)");
        }
    }

    // --- filters: notions §28 ----------------------------------------------
    // filter: ${PARENTID IS NULL}. The ${ } is optional here.
    void readFilter(const YAML::Node& key, const YAML::Node& v, Rule& r) {
        if (!v.IsScalar()) {
            warn(key, "'filter' expects an expression like ${AREA < 30}; "
                      "inside [ ] or { }, put the whole filter between quotes");
            r.invalid = true;
            return;
        }
        std::string text = v.as<std::string>();
        if (text.size() >= 3 && text.compare(0, 2, "${") == 0 && text.back() == '}')
            text = text.substr(2, text.size() - 3);
        std::string error;
        r.filter = Expression::parseCondition(text, error);
        if (!r.filter) {
            warn(key, "filter '" + text + "': " + error + " (the rule is never applied)");
            r.invalid       = true;
            r.invalidFilter = text;
        }
    }

    void readElse(const YAML::Node& key, const YAML::Node& v, Rule& r) {
        try {
            r.isElse = v.as<bool>();
        } catch (const YAML::Exception&) {
            warn(key, "'else' expects true or false (ignored)");
        }
    }

    // --- structure ---------------------------------------------------------
    Rule readRule(const YAML::Node& node) {
        Rule r;
        if (!node.IsMap()) { warn(node, "a rule must be a map"); return r; }
        for (const auto& kv : node) {
            const std::string k = kv.first.as<std::string>();
            if      (k == "name")        r.name = kv.second.as<std::string>("");
            else if (k == "title" || k == "abstract") { /* documentation only */ }
            else if (k == "scale")       r.scale = readScale(kv.first, kv.second);
            else if (k == "symbolizers") readSymbolizers(kv.first, kv.second, r);
            else if (k == "filter")      readFilter(kv.first, kv.second, r);
            else if (k == "else")        readElse(kv.first, kv.second, r);
            else unknownKey(kv.first, "rule");
        }
        if (r.isElse && r.filter) {
            warn(node, "rule '" + r.name + "' has both 'else' and 'filter': the filter is ignored");
            r.filter.reset();
        }
        return r;
    }

    FeatureTypeStyle readFeatureStyle(const YAML::Node& node) {
        FeatureTypeStyle fts;
        if (!node.IsMap()) { warn(node, "a feature-style must be a map"); return fts; }
        for (const auto& kv : node) {
            const std::string k = kv.first.as<std::string>();
            if (k == "rules") {
                if (!kv.second.IsSequence()) { warn(kv.first, "'rules' expects a list"); continue; }
                for (const auto& r : kv.second) fts.rules.push_back(readRule(r));
            } else if (k != "name" && k != "title" && k != "abstract") {
                unknownKey(kv.first, "feature-style");
            }
        }
        return fts;
    }
};

Style Reader::readRoot(const YAML::Node& root) {
    Style style;
    // YSLD lets the outer levels be omitted: a root with 'rules' is one
    // feature-style, a root with 'symbolizers' is one feature-style with one rule.
    for (const auto& kv : root) {
        const std::string k = kv.first.as<std::string>();
        if (k == "name") {
            style.name = kv.second.as<std::string>("");
        } else if (k == "title" || k == "abstract") {
            // documentation only
        } else if (k == "feature-styles") {
            if (!kv.second.IsSequence()) { warn(kv.first, "'feature-styles' expects a list"); continue; }
            for (const auto& f : kv.second) style.featureStyles.push_back(readFeatureStyle(f));
        } else if (k == "rules") {
            FeatureTypeStyle fts;
            if (kv.second.IsSequence()) for (const auto& r : kv.second) fts.rules.push_back(readRule(r));
            else warn(kv.first, "'rules' expects a list");
            style.featureStyles.push_back(std::move(fts));
        } else if (k == "symbolizers") {
            Rule r;
            readSymbolizers(kv.first, kv.second, r);
            style.featureStyles.push_back(FeatureTypeStyle{{std::move(r)}});
        } else {
            unknownKey(kv.first, "style");
        }
    }
    bool anySymbolizer = false;
    for (const auto& f : style.featureStyles)
        for (const auto& r : f.rules) anySymbolizer = anySymbolizer || !r.symbolizers.empty();
    if (!anySymbolizer) warnings_.push_back("the style has no symbolizer: nothing will be drawn");
    return style;
}

} // namespace

YsldResult parseYsld(const std::string& text, const std::string& baseDir) {
    YsldResult res;
    YAML::Node root;
    try {
        root = YAML::Load(text);
    } catch (const YAML::ParserException& e) {
        res.error = "not valid YAML: " + std::string(e.what());
        return res;
    }
    if (!root.IsMap()) {
        res.error = "a YSLD style must be a YAML map (name, feature-styles...)";
        return res;
    }
    Reader reader(res.warnings, baseDir);
    try {
        res.style = reader.readRoot(root);
    } catch (const YAML::Exception& e) {   // e.g. a key that is not a plain word
        res.style.reset();
        res.error = "unexpected YSLD structure: " + std::string(e.what());
    }
    return res;
}

YsldResult readYsldFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        YsldResult res;
        res.error = "cannot read style file '" + path + "'";
        return res;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseYsld(ss.str(), std::filesystem::path(path).parent_path().string());
}

} // namespace r2d
