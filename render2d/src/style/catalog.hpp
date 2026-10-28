// SPDX-License-Identifier: AGPL-3.0-or-later
// The style catalogue (notions §60): what each kind of symbolizer can receive,
// and how to add or remove it. editor2d builds its "+" menus from this list
// (its notions §45); tomorrow the web API will serve it as it is.
#pragma once

#include <string>
#include <vector>

#include "style/style.hpp"

namespace r2d {

enum class SymbolizerKind { Polygon, Line, Point, Text, Raster };   // the variant's order

SymbolizerKind kindOf(const Symbolizer& s);
const char*    kindName(SymbolizerKind kind);   // "polygon", "line", "point", "text", "raster"

// An optional property: its absence changes the drawing (no stroke = no outline).
struct PropertySpec {
    std::string key;     // "stroke", "hatch", "halo", "rotation"...
    std::string label;   // for menus: "Stroke", "Hatch"...
};

// In menu order.
const std::vector<PropertySpec>& optionalProperties(SymbolizerKind kind);

bool hasProperty(const Symbolizer& s, const std::string& key);

// Absent, and what it needs is there (dashes need a stroke).
bool canAddProperty(const Symbolizer& s, const std::string& key);

// With a starting value that shows (§60). No effect when present or unknown.
void addProperty(Symbolizer& s, const std::string& key);

// Removing a stroke removes its dashes with it.
void removeProperty(Symbolizer& s, const std::string& key);

// A new symbolizer with the default style's look (§46), in the layer's colour
// (0 to 3, turning). A text has no label yet: the caller picks a field.
Symbolizer newSymbolizer(SymbolizerKind kind, int colour);

} // namespace r2d
