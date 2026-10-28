// SPDX-License-Identifier: AGPL-3.0-or-later
// Classifying a layer by a field (notions §62): what the field holds, where
// to cut, and the rules of editor2d's assistants Categorize and Graduate
// (editor2d notions §48 to §51). No ImGui, no GDAL: a FeatureSource is read.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/attributes.hpp"
#include "core/feature_source.hpp"
#include "style/style.hpp"

namespace r2d {

// What a field holds, read once over the whole layer.
struct FieldSummary {
    bool        found = false;        // the field exists
    FieldType   type  = FieldType::Text;
    int         total = 0;            // features read
    int         nulls = 0;            // without a value
    std::vector<std::pair<std::string, int>> distinct;   // value, count; sorted (numbers as numbers)
    bool        tooMany = false;      // more than maxDistinct values: `distinct` is cut
    std::vector<double> numbers;      // a number field: every value, sorted
};

FieldSummary summarizeField(FeatureSource& source, const std::string& field, int maxDistinct = 50);

enum class BreakMethod { EqualIntervals, Quantiles };

// The n - 1 inner bounds of n classes (§62). Equal bounds, and bounds at or
// below the smallest value, are dropped: fewer classes, never an empty one.
std::vector<double> classBreaks(const std::vector<double>& sorted, int n, BreakMethod method);

// Component by component, t from 0 (from) to 1 (to). §62, editor2d §49.
Color rampColour(const Color& from, const Color& to, double t);

// The 12 colours of ColorBrewer's "Set3", made to be told apart; i turns.
Color categoryColour(int i);

// A value as written in a filter: 'immeuble' (text, '' for a quote), 12, 3.5.
std::string filterLiteral(const std::string& value, FieldType type);

// The main colour of a symbolizer: a polygon's fill (created if missing), a
// line's stroke, a point's fill, a text's colour.
void setMainColour(Symbolizer& s, const Color& c);

// One rule per value, then "(other)" (else, grey). §62, editor2d §48.
std::vector<Rule> categorize(const Symbolizer& model, const std::string& field, FieldType type,
                             const std::vector<std::string>& values);

// One rule per class (bounds: the inner ones, from classBreaks), the first and
// the last open, then "(other)" for the features without a value. §62, editor2d §49.
std::vector<Rule> graduate(const Symbolizer& model, const std::string& field, const std::vector<double>& bounds,
                           const Color& from, const Color& to);

// ---------------------------------------------------------------------------
// The whole assistant, for editor2d and for `render2d --categorize/--graduate`
// (§63): the same result from both.
// ---------------------------------------------------------------------------

struct ClassifySettings {
    bool        graduate = false;      // false: categorize
    std::string field;
    int         classes  = 5;          // graduate: 2 to 10
    BreakMethod method   = BreakMethod::EqualIntervals;
    Color       from     = Color::fromHex(0xFFF5EB);   // graduate: the light end (editor2d §49)
    Color       to       = Color::fromHex(0x8C2D04);   // the dark end
    int         maxDistinct = 50;      // categorize: beyond, refused (editor2d §48)
};

// One line of the legend to come: a rule, how many features it takes, its colour.
struct LegendEntry {
    std::string name;
    int         count = 0;
    Color       colour;
};

struct Classified {
    bool        ok = false;
    std::string error;                 // when !ok: why (no number, too many values...)
    Style       style;                 // the new style
    std::vector<LegendEntry> legend;   // the class rules and (other), in order
};

// editor2d §50: the first symbolizer of `base` that is not a text is the model
// (`fallback` if there is none), its rule gives the scales; each class gets a
// copy, only its colour changes; the labels (text) go in a last rule of their
// own. `summary`: from summarizeField(..., settings.maxDistinct).
Classified classify(const Style& base, const Symbolizer& fallback, const FieldSummary& summary,
                    const ClassifySettings& settings);

} // namespace r2d
