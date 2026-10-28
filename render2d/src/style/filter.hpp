// SPDX-License-Identifier: AGPL-3.0-or-later
// The filter of a rule: a CQL condition on a feature's attributes.
// Read once into a tree, bound once per pass (names -> columns), evaluated
// for every feature. No GDAL here: it works on an Attributes row.
// See notions §28 and §29.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "core/attributes.hpp"

namespace r2d {

// Three-valued logic, as in SQL: a comparison with NULL is Unknown. §28.
enum class Tri { False, True, Unknown };

struct FilterStats {
    std::size_t notNumber = 0;   // values that could not be read as a number ('75B' < 100)
};

class Filter {
public:
    // Returns nullopt and sets `error` ("column 7: ...") when the text cannot be read.
    // The text is the expression itself, without the ${ } of YSLD.
    static std::optional<Filter> parse(const std::string& text, std::string& error);

    const std::string& text() const { return text_; }

    // Field names used, in order of first appearance.
    std::vector<std::string> fieldNames() const;

    // Replaces names by column numbers, once per pass. Returns the names not
    // found in `fields`: they evaluate as NULL.
    std::vector<std::string> bind(const std::vector<FieldDef>& fields);

    // Columns used once bound (for FeatureSource::setNeededFields).
    std::vector<int> columns() const;

    Tri eval(const Attributes& row, FilterStats& stats) const;

    // The tree, as (AND (>= AREA 30) (< AREA 100)): for tests and debugging.
    std::string dump() const;

    // Implementation detail, public only so that the .cpp helpers can see it.
    enum class Kind { Field, Number, Text, Compare, Between, In, Like, IsNull, Not, And, Or };
    enum class Op { Eq, Ne, Lt, Le, Gt, Ge };
    struct Node {
        Kind             kind   = Kind::Number;
        Op               op     = Op::Eq;
        bool             negate = false;   // IS NOT NULL, NOT BETWEEN, NOT IN, NOT LIKE
        double           number = 0.0;     // Number
        std::string      text;             // Field name, Text literal, Like pattern
        int              column = -1;      // Field, once bound; -1 = unknown -> NULL
        std::vector<int> kids;             // indices in nodes_
    };

private:
    std::vector<Node> nodes_;
    int               root_ = -1;
    std::string       text_;

    friend class FilterParser;
    friend class FilterEval;
};

} // namespace r2d
