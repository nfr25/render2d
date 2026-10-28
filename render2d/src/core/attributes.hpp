// SPDX-License-Identifier: AGPL-3.0-or-later
// The attributes of a feature: one value per field of its layer.
// Three types are enough for styling, plus NULL. See notions §27.
#pragma once

#include <string>
#include <variant>
#include <vector>

namespace r2d {

enum class FieldType { Integer, Real, Text };

struct FieldDef {
    std::string name;
    FieldType   type = FieldType::Text;
};

// std::monostate = NULL (no value), not 0 and not an empty text.
using Value = std::variant<std::monostate, long long, double, std::string>;

inline bool isNull(const Value& v) { return std::holds_alternative<std::monostate>(v); }

// One feature's values, indexed like the layer's fields. Fields that were not
// asked for (FeatureSource::setNeededFields) stay NULL.
using Attributes = std::vector<Value>;

} // namespace r2d
