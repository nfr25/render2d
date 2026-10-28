// SPDX-License-Identifier: AGPL-3.0-or-later
#include "style/catalog.hpp"

#include <cstdint>

namespace r2d {

namespace {

// The starting values (§60): visible at once, adjusted afterwards.
const Color kDark = Color::fromHex(0x333333);

StrokeStyle newStroke() { return StrokeStyle{.color = kDark, .width = 1.0}; }

Expression value(const char* text) {
    std::string error;
    return *Expression::parseValue(text, error);   // our own constants: always readable
}

std::string number(double x) {
    std::string s = std::to_string(x);
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// The default style's palette (§46), one entry per colour number.
const std::uint32_t kFills[]        = {0xF2EBDD, 0xC98B6B, 0xA9C4A0, 0xB7C7DC};
const std::uint32_t kOutlines[]     = {0x9A9286, 0x7A4A35, 0x5F7D57, 0x55708F};
const std::uint32_t kLines[]        = {0x3B6EA5, 0x555A62, 0xB0533C, 0x5F7D57};

int slot(int colour) { return ((colour % 4) + 4) % 4; }

} // namespace

SymbolizerKind kindOf(const Symbolizer& s) {
    return static_cast<SymbolizerKind>(s.index());   // same order as the variant
}

const char* kindName(SymbolizerKind kind) {
    switch (kind) {
        case SymbolizerKind::Polygon: return "polygon";
        case SymbolizerKind::Line:    return "line";
        case SymbolizerKind::Point:   return "point";
        case SymbolizerKind::Text:    return "text";
        case SymbolizerKind::Raster:  return "raster";
    }
    return "?";
}

const std::vector<PropertySpec>& optionalProperties(SymbolizerKind kind) {
    static const std::vector<PropertySpec> polygon = {
        {"fill", "Fill"}, {"stroke", "Stroke"}, {"hatch", "Hatch"}, {"dashes", "Dashes"}};
    static const std::vector<PropertySpec> line = {{"dashes", "Dashes"}};
    static const std::vector<PropertySpec> point = {
        // No fill / stroke here: a mark without them gets SLD's grey and
        // black when read again, so they are always there (§60).
        {"rotation", "Rotation"}, {"sizeField", "Size by field"}, {"metres", "Size in metres"}};
    static const std::vector<PropertySpec> text = {{"halo", "Halo"}, {"rotation", "Rotation"}};
    static const std::vector<PropertySpec> raster = {};   // opacity is always there (§66)
    switch (kind) {
        case SymbolizerKind::Polygon: return polygon;
        case SymbolizerKind::Line:    return line;
        case SymbolizerKind::Point:   return point;
        case SymbolizerKind::Text:    return text;
        case SymbolizerKind::Raster:  return raster;
    }
    return text;
}

bool hasProperty(const Symbolizer& s, const std::string& key) {
    if (auto* p = std::get_if<PolygonSymbolizer>(&s)) {
        if (key == "fill")   return p->fill.has_value();
        if (key == "stroke") return p->stroke.has_value();
        if (key == "hatch")  return p->graphicFill.has_value();
        if (key == "dashes") return p->stroke && !p->stroke->dashes.empty();
    } else if (auto* l = std::get_if<LineSymbolizer>(&s)) {
        if (key == "dashes") return !l->stroke.dashes.empty();
    } else if (auto* pt = std::get_if<PointSymbolizer>(&s)) {
        if (key == "rotation")  return pt->rotation.has_value();
        if (key == "sizeField") return pt->sizeExpr.has_value();
        if (key == "metres")    return pt->metres;
    } else if (auto* t = std::get_if<TextSymbolizer>(&s)) {
        if (key == "halo")     return t->halo.has_value();
        if (key == "rotation") return t->rotation.has_value();
    }
    return false;
}

bool canAddProperty(const Symbolizer& s, const std::string& key) {
    bool known = false;
    for (const PropertySpec& p : optionalProperties(kindOf(s))) known = known || p.key == key;
    if (!known || hasProperty(s, key)) return false;
    if (auto* p = std::get_if<PolygonSymbolizer>(&s); p && key == "dashes") return p->stroke.has_value();
    return true;
}

void addProperty(Symbolizer& s, const std::string& key) {
    if (!canAddProperty(s, key)) return;
    if (auto* p = std::get_if<PolygonSymbolizer>(&s)) {
        if (key == "fill")   p->fill = FillStyle{.color = Color::fromHex(0xDDDDDD)};
        if (key == "stroke") p->stroke = newStroke();
        if (key == "hatch")  p->graphicFill = GraphicFill{.size = 8.0, .shape = MarkShape::Slash,
                                                          .stroke = newStroke()};
        if (key == "dashes") p->stroke->dashes = {4.0, 4.0};
    } else if (auto* l = std::get_if<LineSymbolizer>(&s)) {
        if (key == "dashes") l->stroke.dashes = {4.0, 4.0};
    } else if (auto* pt = std::get_if<PointSymbolizer>(&s)) {
        if (key == "rotation")  pt->rotation = value("0");
        if (key == "sizeField") pt->sizeExpr = value(number(pt->size).c_str());   // the same size, as an expression
        if (key == "metres")    pt->metres = true;
    } else if (auto* t = std::get_if<TextSymbolizer>(&s)) {
        if (key == "halo")     t->halo = Halo{.radius = 1.5};
        if (key == "rotation") t->rotation = value("0");
    }
}

void removeProperty(Symbolizer& s, const std::string& key) {
    if (auto* p = std::get_if<PolygonSymbolizer>(&s)) {
        if (key == "fill")   p->fill.reset();
        if (key == "stroke") p->stroke.reset();
        if (key == "hatch")  p->graphicFill.reset();
        if (key == "dashes" && p->stroke) { p->stroke->dashes.clear(); p->stroke->dashOffset = 0.0; }
    } else if (auto* l = std::get_if<LineSymbolizer>(&s)) {
        if (key == "dashes") { l->stroke.dashes.clear(); l->stroke.dashOffset = 0.0; }
    } else if (auto* pt = std::get_if<PointSymbolizer>(&s)) {
        if (key == "rotation")  pt->rotation.reset();
        if (key == "sizeField") pt->sizeExpr.reset();
        if (key == "metres")    pt->metres = false;
    } else if (auto* t = std::get_if<TextSymbolizer>(&s)) {
        if (key == "halo")     t->halo.reset();
        if (key == "rotation") t->rotation.reset();
    }
}

Symbolizer newSymbolizer(SymbolizerKind kind, int colour) {
    const int i = slot(colour);
    switch (kind) {
        case SymbolizerKind::Polygon:
            // Pale fill with a thin outline, then stronger fills for the next
            // layers (e.g. parcels first, buildings on top).
            return PolygonSymbolizer{
                .fill   = FillStyle{.color = Color::fromHex(kFills[i])},
                .stroke = StrokeStyle{.color = Color::fromHex(kOutlines[i]), .width = 0.8, .join = LineJoin::Round}};
        case SymbolizerKind::Line:
            return LineSymbolizer{StrokeStyle{.color = Color::fromHex(kLines[i]), .width = 1.5,
                                              .cap = LineCap::Round, .join = LineJoin::Round}};
        case SymbolizerKind::Point:
            return PointSymbolizer{.size = 6.0, .fill = FillStyle{.color = Color::fromHex(0xB0333C)},
                                   .stroke = StrokeStyle{.color = Color::fromHex(0xFFFFFF), .width = 1.0}};
        case SymbolizerKind::Text:
        {
            TextSymbolizer t;
            t.halo = Halo{.radius = 1.5};
            return t;
        }
        case SymbolizerKind::Raster:
            return RasterSymbolizer{};
    }
    return TextSymbolizer{};
}

} // namespace r2d
