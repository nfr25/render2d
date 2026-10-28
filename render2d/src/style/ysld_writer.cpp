// SPDX-License-Identifier: AGPL-3.0-or-later
#include "style/ysld_writer.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>

namespace r2d {

namespace {

// The shortest text that reads back as the same double: 0.8, 15000, 1.5.
std::string num(double v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

// 'text' with ' doubled: always safe in YAML, whatever the text holds
// (#, :, [, {, spaces...).
std::string quoted(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("''") : std::string(1, c);
    return out + "'";
}

// '#RRGGBB' (the alpha goes in a separate *-opacity key).
std::string hex(const Color& c) {
    auto byte = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    char buf[16];
    std::snprintf(buf, sizeof buf, "'#%02X%02X%02X'", byte(c.r), byte(c.g), byte(c.b));
    return buf;
}

bool sameColour(const Color& a, const Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

const char* shapeName(MarkShape s) {
    switch (s) {
        case MarkShape::Circle:    return "circle";
        case MarkShape::Square:    return "square";
        case MarkShape::Triangle:  return "triangle";
        case MarkShape::Star:      return "star";
        case MarkShape::Cross:     return "cross";
        case MarkShape::X:         return "x";
        case MarkShape::Slash:     return "shape://slash";
        case MarkShape::Backslash: return "shape://backslash";
        case MarkShape::HorLine:   return "shape://horline";
        case MarkShape::VertLine:  return "shape://vertline";
        case MarkShape::Times:     return "shape://times";
        case MarkShape::Dot:       return "shape://dot";
    }
    return "circle";
}

// ${...}, quoted (it may hold < or :).
std::string expr(const Expression& e) { return quoted("${" + e.text() + "}"); }

class Writer {
public:
    explicit Writer(std::string baseDir) : baseDir_(std::move(baseDir)) {}

    std::string write(const Style& style) {
        if (!style.name.empty()) line(0, "name: " + quoted(style.name));
        if (style.featureStyles.empty()) { line(0, "feature-styles: []"); return out_.str(); }
        line(0, "feature-styles:");
        for (const FeatureTypeStyle& fts : style.featureStyles) {
            if (fts.rules.empty()) { line(0, "- rules: []"); continue; }
            line(0, "- rules:");
            for (const Rule& r : fts.rules) rule(r);
        }
        return out_.str();
    }

private:
    std::ostringstream out_;
    std::string        baseDir_;

    void line(int indent, const std::string& text) { out_ << std::string(static_cast<std::size_t>(indent), ' ') << text << '\n'; }

    // The first key of a list item gets the "- ", the others line up under it.
    struct Item {
        Writer& w;
        int     indent;
        bool    first = true;
        void operator()(const std::string& text) {
            w.line(indent, (first ? "- " : "  ") + text);
            first = false;
        }
    };

    void rule(const Rule& r) {
        Item key{*this, 2};
        if (!r.name.empty()) key("name: " + quoted(r.name));
        const ScaleRange def;
        if (r.scale.min != def.min || r.scale.max != def.max) {
            const std::string lo = r.scale.min == 0.0 ? "min" : num(r.scale.min);
            const std::string hi = std::isinf(r.scale.max) ? "max" : num(r.scale.max);
            key("scale: [" + lo + ", " + hi + "]");
        }
        if (r.isElse) key("else: true");
        else if (r.filter) key("filter: " + expr(*r.filter));
        else if (r.invalid && !r.invalidFilter.empty()) key("filter: " + quoted("${" + r.invalidFilter + "}"));
        if (r.symbolizers.empty()) { key("symbolizers: []"); return; }
        key("symbolizers:");
        for (const Symbolizer& s : r.symbolizers) {
            std::visit([&](const auto& sym) { symbolizer(sym); }, s);
        }
    }

    // --- strokes, shared ------------------------------------------------
    // Every stroke-* key that differs from StrokeStyle's defaults (width 1,
    // butt, miter, solid). The colour is always written: it makes the stroke.
    void stroke(int indent, const StrokeStyle& s) {
        const StrokeStyle def;
        line(indent, "stroke-color: " + hex(s.color));
        if (s.color.a != 1.0) line(indent, "stroke-opacity: " + num(s.color.a));
        if (s.width != def.width) line(indent, "stroke-width: " + num(s.width));
        if (s.cap == LineCap::Round)  line(indent, "stroke-linecap: round");
        if (s.cap == LineCap::Square) line(indent, "stroke-linecap: square");
        if (s.join == LineJoin::Round) line(indent, "stroke-linejoin: round");
        if (s.join == LineJoin::Bevel) line(indent, "stroke-linejoin: bevel");
        if (!s.dashes.empty()) {
            std::string d;
            for (double x : s.dashes) d += (d.empty() ? "" : ", ") + num(x);
            line(indent, "stroke-dasharray: [" + d + "]");
        }
        if (s.dashOffset != def.dashOffset) line(indent, "stroke-dashoffset: " + num(s.dashOffset));
    }

    void fill(int indent, const Color& c) {
        line(indent, "fill-color: " + hex(c));
        if (c.a != 1.0) line(indent, "fill-opacity: " + num(c.a));
    }

    // A mark of a point or of a fill-graphic. The reader gives a point's mark
    // a grey fill and a black outline by default; a fill-graphic's mark has
    // no fill by default.
    void mark(int indent, MarkShape shape, const std::optional<FillStyle>& f,
              const std::optional<StrokeStyle>& s) {
        line(indent, "shape: " + quoted(shapeName(shape)));
        if (f) fill(indent, f->color);
        if (s) stroke(indent, *s);
    }

    // --- symbolizers -------------------------------------------------------
    void symbolizer(const PolygonSymbolizer& p) {
        line(4, "- polygon:");
        if (p.fill) fill(8, p.fill->color);
        if (p.graphicFill) {
            const GraphicFill& g = *p.graphicFill;
            line(8, "fill-graphic:");
            if (g.size != GraphicFill{}.size) line(10, "size: " + num(g.size));
            line(10, "symbols:");
            line(10, "- mark:");
            mark(14, g.shape, g.fill, g.stroke);
        }
        if (p.stroke) stroke(8, *p.stroke);
    }

    void symbolizer(const LineSymbolizer& l) {
        line(4, "- line:");
        stroke(8, l.stroke);
    }

    void symbolizer(const PointSymbolizer& p) {
        line(4, "- point:");
        // size: an expression, a number, or nothing for an image at its own size.
        if (p.sizeExpr) line(8, "size: " + expr(*p.sizeExpr));
        else if (p.image.empty() ? p.size != PointSymbolizer{}.size : p.size != 0.0) line(8, "size: " + num(p.size));
        if (p.rotation) line(8, "rotation: " + expr(*p.rotation));
        if (p.metres) line(8, "uom: metre");
        line(8, "symbols:");
        if (!p.image.empty()) {
            line(8, "- external:");
            line(12, "url: " + quoted(imagePath(p.image)));
            line(12, "format: image/png");
        }
        // The mark: always written without an image; with one, only when it
        // is not the reader's default (grey, black outline, circle).
        const bool defaultMark = p.shape == MarkShape::Circle && p.fill && p.stroke &&
                                 sameColour(p.fill->color, Color::fromHex(0x808080)) &&
                                 sameColour(p.stroke->color, Color::fromHex(0x000000)) &&
                                 p.stroke->width == 1.0 && p.stroke->cap == LineCap::Butt &&
                                 p.stroke->join == LineJoin::Miter && p.stroke->dashes.empty() &&
                                 p.stroke->dashOffset == 0.0;
        if (p.image.empty() || !defaultMark) {
            line(8, "- mark:");
            mark(12, p.shape, p.fill, p.stroke);
        }
    }

    void symbolizer(const TextSymbolizer& t) {
        const TextSymbolizer def;
        line(4, "- text:");
        if (t.label) line(8, "label: " + (t.label->isLiteralText() ? quoted(t.label->text()) : expr(*t.label)));
        if (t.font.family != def.font.family) line(8, "font-family: " + quoted(t.font.family));
        if (t.font.size != def.font.size) line(8, "font-size: " + num(t.font.size));
        if (t.font.bold) line(8, "font-weight: bold");
        if (t.font.italic) line(8, "font-style: italic");
        if (!sameColour(t.fill, def.fill)) {
            Color c = t.fill;
            line(8, "fill-color: " + hex(c));
            if (c.a != 1.0) line(8, "fill-opacity: " + num(c.a));
        }
        if (t.halo) {
            const Halo hdef;
            line(8, "halo:");
            if (t.halo->radius != hdef.radius) line(10, "radius: " + num(t.halo->radius));
            line(10, "fill-color: " + hex(t.halo->color));
            if (t.halo->color.a != 1.0) line(10, "fill-opacity: " + num(t.halo->color.a));
        }
        if (t.anchorX != def.anchorX || t.anchorY != def.anchorY)
            line(8, "anchor: [" + num(t.anchorX) + ", " + num(t.anchorY) + "]");
        if (t.displacementX != 0.0 || t.displacementY != 0.0)
            line(8, "displacement: [" + num(t.displacementX) + ", " + num(t.displacementY) + "]");
        if (t.rotation) line(8, "rotation: " + expr(*t.rotation));
    }

    void symbolizer(const RasterSymbolizer& r) {
        line(4, "- raster:");
        if (r.opacity != RasterSymbolizer{}.opacity) line(8, "opacity: " + num(r.opacity));
    }

    // An image under the folder the style is saved in: written relative to it.
    std::string imagePath(const std::string& path) const {
        namespace fs = std::filesystem;
        if (baseDir_.empty()) return fs::path(path).generic_string();
        const fs::path rel = fs::path(path).lexically_relative(fs::path(baseDir_));
        if (rel.empty() || *rel.begin() == "..") return fs::path(path).generic_string();
        return rel.generic_string();
    }
};

} // namespace

std::string writeYsld(const Style& style, const std::string& baseDir) {
    return Writer(baseDir).write(style);
}

} // namespace r2d
