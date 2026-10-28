// SPDX-License-Identifier: AGPL-3.0-or-later
// CQL expressions on a feature's attributes: conditions for the filters of
// rules (true / false / unknown), and values for labels and rotations.
// Read once into a tree, bound once per pass (names -> columns), evaluated
// for every feature. No GDAL here: it works on an Attributes row.
// See notions §28, §29, §35 and §36.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "core/attributes.hpp"

namespace r2d {

// Three-valued logic, as in SQL: a comparison with NULL is Unknown. §28.
enum class Tri { False, True, Unknown };

struct EvalStats {
    std::size_t notNumber = 0;   // values that could not be read as a number ('75B' < 100)
};

class Expression {
public:
    // A condition: PARENTID IS NULL, AREA >= 30 AND AREA < 100... (filters).
    // Returns nullopt and sets `error` ("column 7: ...") when the text cannot be read.
    // The text is the expression itself, without the ${ } of YSLD.
    static std::optional<Expression> parseCondition(const std::string& text, std::string& error);

    // A value: POLICENUM, 360 - CARTOANGLE, 4 * sqrt(N), 'Evere'... (labels, rotations, sizes).
    // Functions: sqrt, abs (notions §41).
    static std::optional<Expression> parseValue(const std::string& text, std::string& error);

    // A fixed text, for label: Evere written without ${ }.
    static Expression literalText(const std::string& text);

    const std::string& text() const { return text_; }

    // A fixed text made by literalText (label: Evere, no ${ }): the writer
    // writes it back as it was (§59).
    bool isLiteralText() const;

    // Field names used, in order of first appearance.
    std::vector<std::string> fieldNames() const;

    // Replaces names by column numbers, once per pass. Returns the names not
    // found in `fields`: they evaluate as NULL.
    std::vector<std::string> bind(const std::vector<FieldDef>& fields);

    // Columns used once bound (for FeatureSource::setNeededFields).
    std::vector<int> columns() const;

    // For a condition.
    Tri test(const Attributes& row, EvalStats& stats) const;

    // For a value. Arithmetic on a NULL, on a text that is not a number, or a
    // division by zero gives NULL. A condition gives 1 (true), 0 (false) or NULL.
    Value value(const Attributes& row, EvalStats& stats) const;

    // The tree, as (AND (>= AREA 30) (< AREA 100)): for tests and debugging.
    std::string dump() const;

    // Implementation detail, public only so that the .cpp helpers can see it.
    enum class Kind { Field, Number, Text, Arith, Neg, Func,                // values
                      Compare, Between, In, Like, IsNull, Not, And, Or };   // conditions
    enum class Op { Eq, Ne, Lt, Le, Gt, Ge };
    struct Node {
        Kind             kind   = Kind::Number;
        Op               op     = Op::Eq;
        char             arith  = '+';     // Arith: + - * /
        bool             negate = false;   // IS NOT NULL, NOT BETWEEN, NOT IN, NOT LIKE
        double           number = 0.0;     // Number
        std::string      text;             // Field name, Text literal, Like pattern, Func name
        int              column = -1;      // Field, once bound; -1 = unknown -> NULL
        std::vector<int> kids;             // indices in nodes_
        std::size_t      pos    = 0;       // in the text, for error messages
    };

private:
    std::vector<Node> nodes_;
    int               root_ = -1;
    std::string       text_;

    friend class ExprParser;
    friend class ExprEval;
};

} // namespace r2d
