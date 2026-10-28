// SPDX-License-Identifier: AGPL-3.0-or-later
#include "style/classify.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>

namespace r2d {

namespace {

const Color kOther = Color::fromHex(0xCCCCCC);

// 6 significant digits: 606.8399999999999 (the computer's binary fractions)
// becomes 606.84, read in the legend and in the filter alike.
double roundSignificant(double x, int digits) {
    if (x == 0.0 || !std::isfinite(x)) return x;
    const double scale = std::pow(10.0, digits - 1 - static_cast<int>(std::floor(std::log10(std::fabs(x)))));
    return std::round(x * scale) / scale;
}

std::string number(double x) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, x);
    return std::string(buf, r.ptr);
}

Expression condition(const std::string& text) {
    std::string error;
    return *Expression::parseCondition(text, error);   // built here: always readable
}

Rule otherRule(const Symbolizer& model) {
    Rule r;
    r.name   = "(other)";
    r.isElse = true;
    Symbolizer s = model;
    setMainColour(s, kOther);
    r.symbolizers.push_back(std::move(s));
    return r;
}

} // namespace

FieldSummary summarizeField(FeatureSource& source, const std::string& field, int maxDistinct) {
    FieldSummary out;
    const std::vector<FieldDef>& defs = source.fields();
    int column = -1;
    for (std::size_t i = 0; i < defs.size(); ++i)
        if (defs[i].name == field) column = static_cast<int>(i);
    if (column < 0) return out;
    out.found = true;
    out.type  = defs[static_cast<std::size_t>(column)].type;

    // Counted in a map: text in alphabetical order, numbers in numeric order.
    std::map<std::string, int> texts;
    std::map<double, int>      values;
    source.setNeededFields({column});
    source.forEach([&](const Geometry&, const Attributes& a, FeatureId) {
        ++out.total;
        const Value& v = a[static_cast<std::size_t>(column)];
        if (isNull(v)) { ++out.nulls; return; }
        if (const auto* i = std::get_if<long long>(&v)) {
            out.numbers.push_back(static_cast<double>(*i));
            ++values[static_cast<double>(*i)];
        } else if (const auto* d = std::get_if<double>(&v)) {
            out.numbers.push_back(*d);
            ++values[*d];
        } else {
            ++texts[std::get<std::string>(v)];
        }
    });
    std::sort(out.numbers.begin(), out.numbers.end());
    auto keep = [&](std::string value, int count) {
        if (static_cast<int>(out.distinct.size()) >= maxDistinct) { out.tooMany = true; return; }
        out.distinct.emplace_back(std::move(value), count);
    };
    for (const auto& [v, n] : values) {
        if (out.type == FieldType::Integer) keep(std::to_string(static_cast<long long>(v)), n);
        else keep(number(v), n);
    }
    for (const auto& [t, n] : texts) keep(t, n);
    return out;
}

std::vector<double> classBreaks(const std::vector<double>& sorted, int n, BreakMethod method) {
    std::vector<double> bounds;
    if (sorted.empty() || n < 2) return bounds;
    const double lo = sorted.front(), hi = sorted.back();
    const std::size_t count = sorted.size();
    for (int k = 1; k < n; ++k) {
        double b;
        if (method == BreakMethod::EqualIntervals) {
            b = roundSignificant(lo + (hi - lo) * k / n, 6);   // (900 - 30) / 3 = 290: 320, 610
        } else {
            // Quantiles: the value of rank floor(k N / n), counted from 0: 80, 220.
            const std::size_t rank = static_cast<std::size_t>(k) * count / static_cast<std::size_t>(n);
            b = sorted[std::min(rank, count - 1)];
        }
        if (b <= lo) continue;                              // the first class would be empty
        if (!bounds.empty() && b <= bounds.back()) continue;  // the same bound twice
        bounds.push_back(b);
    }
    return bounds;
}

Color rampColour(const Color& from, const Color& to, double t) {
    t = std::clamp(t, 0.0, 1.0);
    // In bytes, rounded: 197.5 -> 198, as in editor2d §49.
    auto mix = [t](double a, double b) {
        return std::floor((a + t * (b - a)) * 255.0 + 0.5) / 255.0;
    };
    return {mix(from.r, to.r), mix(from.g, to.g), mix(from.b, to.b), from.a + t * (to.a - from.a)};
}

Color categoryColour(int i) {
    static const std::uint32_t set3[] = {0x8DD3C7, 0xFFFFB3, 0xBEBADA, 0xFB8072, 0x80B1D3, 0xFDB462,
                                         0xB3DE69, 0xFCCDE5, 0xD9D9D9, 0xBC80BD, 0xCCEBC5, 0xFFED6F};
    return Color::fromHex(set3[((i % 12) + 12) % 12]);
}

std::string filterLiteral(const std::string& value, FieldType type) {
    if (type != FieldType::Text) return value;
    std::string out = "'";
    for (char c : value) out += (c == '\'' ? std::string("''") : std::string(1, c));
    return out + "'";
}

void setMainColour(Symbolizer& s, const Color& c) {
    if (auto* p = std::get_if<PolygonSymbolizer>(&s)) {
        if (!p->fill) p->fill = FillStyle{};
        p->fill->color = c;
    } else if (auto* l = std::get_if<LineSymbolizer>(&s)) {
        l->stroke.color = c;
    } else if (auto* pt = std::get_if<PointSymbolizer>(&s)) {
        if (!pt->fill) pt->fill = FillStyle{};
        pt->fill->color = c;
    } else if (auto* t = std::get_if<TextSymbolizer>(&s)) {
        t->fill = c;
    }
}

std::vector<Rule> categorize(const Symbolizer& model, const std::string& field, FieldType type,
                             const std::vector<std::string>& values) {
    std::vector<Rule> rules;
    for (std::size_t i = 0; i < values.size(); ++i) {
        Rule r;
        r.name   = values[i];
        r.filter = condition(field + " = " + filterLiteral(values[i], type));
        Symbolizer s = model;
        setMainColour(s, categoryColour(static_cast<int>(i)));
        r.symbolizers.push_back(std::move(s));
        rules.push_back(std::move(r));
    }
    rules.push_back(otherRule(model));
    return rules;
}

std::vector<Rule> graduate(const Symbolizer& model, const std::string& field, const std::vector<double>& bounds,
                           const Color& from, const Color& to) {
    std::vector<Rule> rules;
    const std::size_t n = bounds.size() + 1;   // classes
    for (std::size_t k = 0; k < n; ++k) {
        Rule r;
        std::string text;
        if (n == 1) {
            text   = field + " IS NOT NULL";
            r.name = "all";
        } else if (k == 0) {
            text   = field + " < " + number(bounds[0]);
            r.name = "< " + number(bounds[0]);
        } else if (k == n - 1) {
            text   = field + " >= " + number(bounds[k - 1]);
            r.name = ">= " + number(bounds[k - 1]);
        } else {
            text   = field + " >= " + number(bounds[k - 1]) + " AND " + field + " < " + number(bounds[k]);
            r.name = number(bounds[k - 1]) + " - " + number(bounds[k]);
        }
        r.filter = condition(text);
        Symbolizer s = model;
        setMainColour(s, rampColour(from, to, n == 1 ? 0.0 : static_cast<double>(k) / static_cast<double>(n - 1)));
        r.symbolizers.push_back(std::move(s));
        rules.push_back(std::move(r));
    }
    rules.push_back(otherRule(model));
    return rules;
}

namespace {

// A band's name from its rule: "proche", or, for a rule an earlier classify
// made, the band at the end of "< 29684 (proche)": classified again, the names
// stay "< 48 (proche)", not "< 48 (< 29684 (proche))".
std::string bandName(const std::string& rule) {
    if (rule.size() < 3 || rule.back() != ')') return rule;
    const auto open = rule.rfind(" (");
    if (open == std::string::npos) return rule;
    return rule.substr(open + 2, rule.size() - open - 3);
}

Color mainColour(const Symbolizer& s) {
    if (const auto* p = std::get_if<PolygonSymbolizer>(&s)) return p->fill ? p->fill->color : Color{};
    if (const auto* l = std::get_if<LineSymbolizer>(&s)) return l->stroke.color;
    if (const auto* pt = std::get_if<PointSymbolizer>(&s)) return pt->fill ? pt->fill->color : Color{};
    if (const auto* t = std::get_if<TextSymbolizer>(&s)) return t->fill;
    return {};
}

} // namespace

Classified classify(const Style& base, const Symbolizer& fallback, const FieldSummary& summary,
                    const ClassifySettings& settings) {
    Classified out;
    if (!summary.found) {
        out.error = "no field '" + settings.field + "'";
        return out;
    }
    if (settings.graduate && summary.numbers.empty()) {
        out.error = "no number in '" + settings.field + "': it cannot be graduated";
        return out;
    }
    if (!settings.graduate && summary.tooMany) {
        out.error = "more than " + std::to_string(settings.maxDistinct) + " values in '" + settings.field +
                    "': a legend that long does not read (graduate a number, or group the values)";
        return out;
    }

    // §63, editor2d §50: the BANDS of the style. Each scale range met is one
    // band; its first rule with a symbolizer that is not a text gives the model.
    struct Band {
        ScaleRange        scale;
        const Symbolizer* model = nullptr;
        std::string       name;
    };
    auto sameScale = [](const ScaleRange& a, const ScaleRange& b) { return a.min == b.min && a.max == b.max; };
    std::vector<Band> bands;
    std::vector<Rule> labelRules;   // the texts, one rule per scale, where they were
    for (const FeatureTypeStyle& fts : base.featureStyles)
        for (const Rule& r : fts.rules) {
            const Symbolizer* model = nullptr;
            Rule texts;
            for (const Symbolizer& sym : r.symbolizers) {
                if (std::holds_alternative<TextSymbolizer>(sym)) texts.symbolizers.push_back(sym);
                else if (!model) model = &sym;
            }
            if (model && std::none_of(bands.begin(), bands.end(), [&](const Band& b) { return sameScale(b.scale, r.scale); }))
                bands.push_back({r.scale, model, bandName(r.name)});
            if (!texts.symbolizers.empty() &&
                std::none_of(labelRules.begin(), labelRules.end(), [&](const Rule& l) { return sameScale(l.scale, r.scale); })) {
                texts.scale = r.scale;
                texts.name  = "labels";
                labelRules.push_back(std::move(texts));
            }
        }
    if (bands.empty()) bands.push_back({ScaleRange{}, &fallback, ""});

    // The class rules of one band; the bounds are computed once for all.
    std::vector<double> bounds;
    std::vector<std::string> values;
    if (settings.graduate) bounds = classBreaks(summary.numbers, settings.classes, settings.method);
    else for (const auto& [v, n] : summary.distinct) values.push_back(v);
    auto classesFor = [&](const Symbolizer& m) {
        return settings.graduate ? graduate(m, settings.field, bounds, settings.from, settings.to)
                                 : categorize(m, settings.field, summary.type, values);
    };

    // The legend, with its counts (the same in every band).
    for (const Rule& r : classesFor(*bands.front().model))
        out.legend.push_back({r.name, 0, mainColour(r.symbolizers.front())});
    out.legend.back().count = summary.nulls;   // (other): no value
    if (settings.graduate) {
        // v is in the class of the number of bounds <= v (each bound in the class above).
        for (double v : summary.numbers) {
            const auto k = static_cast<std::size_t>(std::upper_bound(bounds.begin(), bounds.end(), v) - bounds.begin());
            if (k + 1 < out.legend.size()) ++out.legend[k].count;
        }
    } else {
        for (std::size_t i = 0; i < summary.distinct.size() && i + 1 < out.legend.size(); ++i)
            out.legend[i].count = summary.distinct[i].second;
    }

    // Every band gets the classes, with its own symbol and scales; the band's
    // name follows the class's when there are several ("< 80 (proche)").
    std::vector<Rule> rules;
    for (std::size_t b = 0; b < bands.size(); ++b) {
        // ASCII only: editor2d's font has no other characters.
        const std::string suffix = bands.size() < 2 ? std::string()
                                 : " (" + (bands[b].name.empty() ? "band " + std::to_string(b + 1) : bands[b].name) + ")";
        for (Rule& r : classesFor(*bands[b].model)) {
            r.scale = bands[b].scale;
            r.name += suffix;
            rules.push_back(std::move(r));
        }
    }
    for (Rule& l : labelRules) {
        if (labelRules.size() > 1) l.name += " " + std::to_string(&l - labelRules.data() + 1);
        rules.push_back(std::move(l));
    }
    out.style.name = base.name.empty() ? settings.field : base.name;
    out.style.featureStyles = {FeatureTypeStyle{std::move(rules)}};
    out.ok = true;
    return out;
}

} // namespace r2d
