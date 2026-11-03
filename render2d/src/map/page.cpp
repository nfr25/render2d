// SPDX-License-Identifier: AGPL-3.0-or-later
#include "map/page.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

#include <map>

#include "backend/gdal/gdal_reproject.hpp"   // GeodesicMeter, northAngle
#include "canvas/canvas.hpp"
#include "render/symbolize.hpp"              // drawSymbolizer, scaledStyle: the map's own drawing (§74, 5b)

namespace r2d {

namespace {

// The page's bands, in millimetres (notions §74, the sketch).
constexpr double kMargin  = 10.0;   // around the sheet
constexpr double kTopBand = 12.0;   // the title
constexpr double kFootBand = 28.0;  // the scale bar, the legend (5b), the north arrow

double mm(double millimetres) { return millimetres / kPointMm; }   // -> points

// The smallest of 1, 2, 2.5, 5 x 10^n that is >= x: a scale that can be written.
double niceUp(double x) {
    const double p = std::pow(10.0, std::floor(std::log10(x)));
    for (double f : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (f * p >= x * (1.0 - 1e-9)) return f * p;
    return 10.0 * p;
}

// The largest of 1, 2, 5 x 10^n that is <= x: a scale bar's length.
double niceDown(double x) {
    const double p = std::pow(10.0, std::floor(std::log10(x)));
    for (double f : {5.0, 2.0, 1.0})
        if (f * p <= x * (1.0 + 1e-9)) return f * p;
    return p;
}

std::string today() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char s[16];
    std::strftime(s, sizeof s, "%Y-%m-%d", &tm);
    return s;
}

// "50 m", "2.5 km", "0".
std::string lengthText(double metres, bool unit) {
    char s[32];
    if (metres >= 1000.0) std::snprintf(s, sizeof s, "%g%s", metres / 1000.0, unit ? " km" : "");
    else                  std::snprintf(s, sizeof s, "%g%s", metres, unit ? " m" : "");
    return s;
}

void rect(Canvas& cv, double x, double y, double w, double h) {
    cv.moveTo({x, y});
    cv.lineTo({x + w, y});
    cv.lineTo({x + w, y + h});
    cv.lineTo({x, y + h});
    cv.closePath();
}

// Text with its baseline starting at (x, y); `anchor` 0 = left, 0.5 = centre, 1 = right.
void text(Canvas& cv, const std::string& s, const FontStyle& font, double x, double y, double anchor,
          const Color& colour) {
    if (s.empty()) return;
    const double w = anchor > 0.0 ? cv.measureText(s, font).advance : 0.0;
    cv.addText(s, font, Affine::translation(x - anchor * w, y));
    cv.fill(FillStyle{colour}, PathMode::Consume);
}

const Color kInk   = Color::fromHex(0x222222);
const Color kGrey  = Color::fromHex(0x666666);
const Color kWhite = Color{1.0, 1.0, 1.0, 1.0};

FontStyle font(double size, bool bold = false) {
    FontStyle f;
    f.size = size;
    f.bold = bold;
    return f;
}

// The scale bar (§74): a round length about a fifth of the frame, in two
// halves, black then white; 0, the middle, the end and its unit below.
void drawScaleBar(Canvas& cv, const PagePlan& plan, double x, double y) {
    const ScaleBar bar = scaleBar(plan);
    const double ground = bar.ground, length = bar.points;
    const double h = mm(2.4);
    rect(cv, x, y, length / 2.0, h);
    cv.fill(FillStyle{kInk}, PathMode::Consume);
    rect(cv, x + length / 2.0, y, length / 2.0, h);
    cv.fill(FillStyle{kWhite}, PathMode::Keep);
    StrokeStyle line;
    line.color = kInk;
    line.width = 0.6;
    cv.stroke(line, PathMode::Consume);
    rect(cv, x, y, length, h);
    cv.stroke(line, PathMode::Consume);
    const FontStyle f = font(7.5);
    const double below = y + h + mm(3.4);
    text(cv, "0", f, x, below, 0.5, kInk);
    text(cv, lengthText(ground / 2.0, false), f, x + length / 2.0, below, 0.5, kInk);
    text(cv, lengthText(ground, true), f, x + length, below, 0.5, kInk);
    text(cv, scaleText(plan.scale), font(9.0), x, below + mm(5.0), 0.0, kInk);
}

// The north arrow (§74), turned towards the true north computed by PROJ.
void drawNorthArrow(Canvas& cv, double cx, double cy, double degrees) {
    const double a = degrees * 3.14159265358979323846 / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    // Clockwise on the page (y downwards): "up" (0, -1) turned by 90° is east.
    auto at = [&](double x, double y) { return Point{cx + mm(x * c - y * s), cy + mm(x * s + y * c)}; };
    cv.moveTo(at(0.0, -7.0));
    cv.lineTo(at(2.6, 3.0));
    cv.lineTo(at(0.0, 1.0));
    cv.lineTo(at(-2.6, 3.0));
    cv.closePath();
    cv.fill(FillStyle{kInk}, PathMode::Consume);
    const Point n = at(0.0, -9.0);
    text(cv, "N", font(9.0, true), n.x, n.y, 0.5, kInk);
}

// ---- the legend (§74, 5b) -----------------------------------------------------------

constexpr double kLegendLine = 4.5;    // mm: a line, a group's title too
constexpr double kLegendCol  = 45.0;   // mm: a column
constexpr double kSwatchW = 7.0, kSwatchH = 3.5;   // mm

// The sample a swatch shows, in page points.
Geometry sample(GeomType type, double x, double y) {
    Geometry g;
    g.type = type;
    const double w = mm(kSwatchW), h = mm(kSwatchH), in = mm(0.4);
    switch (type) {
        case GeomType::Polygon:
            g.points = {{x + in, y + in}, {x + w - in, y + in}, {x + w - in, y + h - in}, {x + in, y + h - in}, {x + in, y + in}};
            break;
        case GeomType::Line:
            g.points = {{x, y + h / 2.0}, {x + w, y + h / 2.0}};
            break;
        case GeomType::Point:
            g.points = {{x + w / 2.0, y + h / 2.0}};
            break;
    }
    g.closePart(0);
    return g;
}

// The text, cut with "…" so that it fits in `width` points.
std::string fitted(Canvas& cv, const std::string& s, const FontStyle& f, double width) {
    if (cv.measureText(s, f).advance <= width) return s;
    std::string t = s;
    while (!t.empty()) {
        do { t.pop_back(); } while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80);   // UTF-8
        if (cv.measureText(t + "\u2026", f).advance <= width) return t + "\u2026";
    }
    return {};
}

// Draws the groups in columns inside (x, y, w, h); returns the lines left out.
int drawLegend(Canvas& cv, const std::vector<LegendGroup>& groups, double x, double y, double w, double h) {
    struct Item { const LegendLine* line; std::string text; };   // line null: a group's title
    std::vector<Item> items;
    for (const LegendGroup& g : groups) {
        if (g.lines.size() > 1) items.push_back({nullptr, g.title});
        for (const LegendLine& l : g.lines) items.push_back({&l, l.label});
    }
    const int rows = static_cast<int>(h / mm(kLegendLine));
    const int cols = static_cast<int>(w / mm(kLegendCol));
    const int room = std::max(0, rows * cols);
    std::map<const void*, int> patterns;   // hatching tiles, once per page (§43)
    SymbolContext ctx;
    ctx.patterns = &patterns;
    const FontStyle plain = font(7.5), bold = font(7.5, true);
    const double textX = mm(kSwatchW + 2.0), textW = mm(kLegendCol - kSwatchW - 4.0);
    for (int k = 0; k < std::min(room, static_cast<int>(items.size())); ++k) {
        const double x0 = x + (k / rows) * mm(kLegendCol), y0 = y + (k % rows) * mm(kLegendLine);
        const Item& it = items[static_cast<std::size_t>(k)];
        if (!it.line) {
            text(cv, fitted(cv, it.text, bold, mm(kLegendCol - 2.0)), bold, x0, y0 + mm(3.4), 0.0, kInk);
            continue;
        }
        const double sy = y0 + mm(0.5);
        cv.save();
        cv.clipRect(x0 - mm(0.5), sy - mm(0.5), mm(kSwatchW + 1.0), mm(kSwatchH + 1.0));   // a thick stroke stays in its swatch
        const Geometry g = sample(it.line->sample, x0, sy);
        for (const Symbolizer& sym : it.line->symbols) drawSymbolizer(cv, Affine::identity(), g, sym, ctx);
        cv.restore();
        text(cv, fitted(cv, it.text, plain, textW), plain, x0 + textX, y0 + mm(3.4), 0.0, kInk);
    }
    return static_cast<int>(items.size()) - std::min(room, static_cast<int>(items.size()));
}

} // namespace

std::vector<LegendGroup> legendEntries(const Map& map, const View& view, double pixelRatio, const MapReport* drawn) {
    std::vector<LegendGroup> groups;
    const double scale = view.scaleDenominator();
    const Envelope& seen = view.shown;
    for (int i = map.layerCount() - 1; i >= 0; --i) {   // top layer first
        if (!map.layerInLegend(i)) continue;
        if (const auto e = map.layerExtent(i);
            e && (e->xmax < seen.xmin || e->xmin > seen.xmax || e->ymax < seen.ymin || e->ymin > seen.ymax))
            continue;   // not on the page
        const LayerInfo& info = map.layerInfo(i);
        const GeomType guess = info.type ? *info.type : GeomType::Polygon;
        const Style style = scaledStyle(map.layerStyle(i, guess), pixelRatio);
        // The rules that drew on the page, by the names the report gives
        // them (an unnamed rule: "pass 2 rule 1").
        // Without filters the report does not count per rule: every active
        // rule then drew every feature, so all of them when one was drawn.
        std::vector<std::string> used;
        bool filtered = false;
        if (drawn) {
            const LayerReport* lr = nullptr;
            for (const LayerReport& r : drawn->layers)
                if (r.layer == i) lr = &r;
            if (!lr || lr->stats.read.drawn == 0) continue;   // nothing of it on the page
            filtered = lr->stats.filtered;
            for (const RuleCount& rc : lr->stats.rules)
                if (rc.drawn > 0) used.push_back(rc.name);
        }
        LegendGroup group;
        group.title = map.layerTitle(i);
        // A style without any filter draws every rule on every feature (a pale
        // ground, then a boundary): one symbol on the map, so one line, its
        // rules stacked, under the layer's title. Classes need filters.
        bool anyFilter = false;
        for (const FeatureTypeStyle& fts : style.featureStyles)
            for (const Rule& rule : fts.rules)
                if (rule.filter || rule.isElse || rule.invalid) anyFilter = true;
        std::vector<std::string> order;            // the rules' names, as first met
        std::map<std::string, LegendLine> lines;
        for (std::size_t p = 0; p < style.featureStyles.size(); ++p) {   // the passes, in order
            const FeatureTypeStyle& fts = style.featureStyles[p];
            for (const Rule* rule : activeRules(fts, scale)) {
                if (filtered) {
                    const std::string counted = rule->name.empty()
                        ? "pass " + std::to_string(p + 1) + " rule " + std::to_string(rule - fts.rules.data() + 1)
                        : rule->name;
                    if (std::find(used.begin(), used.end(), counted) == used.end()) continue;
                }
                std::vector<Symbolizer> syms;
                for (const Symbolizer& sym : rule->symbolizers)
                    if (!std::holds_alternative<TextSymbolizer>(sym) && !std::holds_alternative<RasterSymbolizer>(sym))
                        syms.push_back(sym);
                if (syms.empty()) continue;
                const std::string key = anyFilter ? rule->name : std::string();
                auto [it, fresh] = lines.try_emplace(key);
                if (fresh) {
                    order.push_back(key);
                    it->second.label = rule->name.empty() ? group.title : rule->name;
                    if (info.type) it->second.sample = *info.type;
                    else if (std::holds_alternative<LineSymbolizer>(syms[0])) it->second.sample = GeomType::Line;
                    else if (std::holds_alternative<PointSymbolizer>(syms[0])) it->second.sample = GeomType::Point;
                }
                for (Symbolizer& s : syms) it->second.symbols.push_back(std::move(s));
            }
        }
        for (const std::string& name : order) group.lines.push_back(std::move(lines[name]));
        if (group.lines.empty()) continue;
        if (group.lines.size() == 1) group.lines[0].label = group.title;   // one line: the layer itself
        groups.push_back(std::move(group));
    }
    return groups;
}

std::string legendText(const std::vector<LegendGroup>& groups) {
    std::string out;
    for (const LegendGroup& g : groups) {
        if (!out.empty()) out += "; ";
        out += g.title;
        if (g.lines.size() > 1) {
            out += ":";
            for (std::size_t k = 0; k < g.lines.size(); ++k) out += (k ? ", " : " ") + g.lines[k].label;
        }
    }
    return out;
}

bool parsePageFormat(const std::string& text, PageSpec& spec, std::string& error) {
    std::string t;
    for (char ch : text) t += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const auto dash = t.find('-');
    const std::string size = t.substr(0, dash), turn = dash == std::string::npos ? "portrait" : t.substr(dash + 1);
    if (size != "a4" && size != "a3") {
        error = "page '" + text + "': A4 or A3 expected (A4-landscape, A3-portrait...)";
        return false;
    }
    if (turn != "portrait" && turn != "landscape") {
        error = "page '" + text + "': portrait or landscape expected after the size (A4-landscape)";
        return false;
    }
    spec.format    = size == "a4" ? "A4" : "A3";
    spec.landscape = turn == "landscape";
    return true;
}

std::string pageFormatText(const PageSpec& spec) {
    return spec.format + (spec.landscape ? "-landscape" : "-portrait");
}

ScaleBar scaleBar(const PagePlan& plan) {
    ScaleBar bar;
    bar.ground = niceDown(plan.frameW * kPointMm / 5.0 * plan.scale / 1000.0);   // metres of ground
    bar.points = mm(bar.ground * 1000.0 / plan.scale);                           // on the paper
    return bar;
}

std::string scaleText(double scale) {
    const long long n = std::llround(scale);
    std::string digits = std::to_string(n), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ' ';
        out += digits[i];
    }
    return "1:" + out;
}

FrameMm pageFrame(const PageSpec& spec) {
    double w = spec.format == "A3" ? 297.0 : 210.0, h = spec.format == "A3" ? 420.0 : 297.0;
    if (spec.landscape) std::swap(w, h);
    return {w - 2.0 * kMargin, h - 2.0 * kMargin - kTopBand - kFootBand};
}

double mapPerGroundAt(const CrsInfo& crs, Point p) {
    if (!crs.known) return 1.0;
    const Measurement m = GeodesicMeter(crs).measure({p, {p.x + 100.0, p.y}}, false);   // a 100 m step eastwards
    return m.onGround && m.length > 0.0 ? m.mapLength / m.length : 1.0;
}

double niceScale(double scale) { return niceUp(scale); }

std::optional<PagePlan> planPage(const Map& map, const PageSpec& spec, const std::optional<Envelope>& area,
                                 std::string& error) {
    PagePlan plan;
    // 1. The sheet: A4 = 210 x 297 mm, A3 = 297 x 420 mm.
    double w = spec.format == "A3" ? 297.0 : 210.0, h = spec.format == "A3" ? 420.0 : 297.0;
    if (spec.landscape) std::swap(w, h);
    plan.width  = mm(w);
    plan.height = mm(h);
    const double frameMmW = w - 2.0 * kMargin, frameMmH = h - 2.0 * kMargin - kTopBand - kFootBand;

    // 2. Where: the centre asked for, else the area's (--bbox, else the map's extent).
    const std::optional<Envelope> zone = area ? area : map.extent();
    if (!spec.center && !zone) {
        error = "the page has nothing to show: no centre, no area, no layer with an extent";
        return std::nullopt;
    }
    const Point centre = spec.center ? *spec.center
                                     : Point{(zone->xmin + zone->xmax) / 2.0, (zone->ymin + zone->ymax) / 2.0};

    // 3. Map units per metre of ground at the centre (§74): 1.00004 in
    //    Lambert 72 at Brussels, 1.584 in Web Mercator there.
    const CrsInfo& crs = map.crs();
    std::optional<GeodesicMeter> meter;
    auto mapPerGround = [&](Point p) -> std::optional<double> {   // along a 100 m step eastwards
        const Measurement m = meter->measure({p, {p.x + 100.0, p.y}}, false);
        return m.onGround && m.length > 0.0 ? std::optional<double>(m.mapLength / m.length) : std::nullopt;
    };
    if (crs.known) {
        meter.emplace(crs);
        if (const auto k = mapPerGround(centre)) plan.mapPerGround = *k;
    } else {
        plan.warnings.push_back("the map has no known system: its units are taken for metres of ground");
    }

    // 4. The scale on the ground: asked for, else the nice one that fits the area.
    if (spec.scale) {
        if (!(*spec.scale > 0.0)) {
            error = "the scale must be a positive number (1000 for 1:1 000)";
            return std::nullopt;
        }
        plan.scale = *spec.scale;
    } else {
        if (!zone || !(zone->xmax > zone->xmin) || !(zone->ymax > zone->ymin)) {
            error = "no scale given and no area to fit: give a scale (1000 for 1:1 000) or an area";
            return std::nullopt;
        }
        const double groundW = (zone->xmax - zone->xmin) / plan.mapPerGround;
        const double groundH = (zone->ymax - zone->ymin) / plan.mapPerGround;
        plan.scale = niceUp(std::max(groundW / (frameMmW / 1000.0), groundH / (frameMmH / 1000.0)));
    }

    // 5. The frame in whole points, and the view: map units per point =
    //    ground metres per paper millimetre x millimetres per point x map units per metre.
    const ImageSize size{static_cast<int>(std::lround(mm(frameMmW))), static_cast<int>(std::lround(mm(frameMmH)))};
    const double res = plan.scale / 1000.0 * kPointMm * plan.mapPerGround;
    const Envelope shown{centre.x - size.width * res / 2.0, centre.y - size.height * res / 2.0,
                         centre.x + size.width * res / 2.0, centre.y + size.height * res / 2.0};
    const auto view = makeView(shown, size);
    if (!view) {
        error = "the page's view could not be made";
        return std::nullopt;
    }
    plan.view   = *view;
    plan.view.pixelRatio = kPixelPerPoint;

    // §74: the scale is exact at the centre. Across the page it changes with
    // the system's own scale factor: said when the four edges differ by more
    // than 0.5 % (Web Mercator over all of Belgium; never on a district).
    if (meter) {
        double lo = plan.mapPerGround, hi = plan.mapPerGround;
        for (const Point p : {Point{centre.x, shown.ymin}, Point{centre.x, shown.ymax},
                              Point{shown.xmin, centre.y}, Point{shown.xmax - 100.0, centre.y}})
            if (const auto k = mapPerGround(p)) { lo = std::min(lo, *k); hi = std::max(hi, *k); }
        if (hi / lo - 1.0 > 0.005) {
            char s[200];
            std::snprintf(s, sizeof s, "the scale %s holds at the centre: across the page it changes by %.1f %% "
                          "(%s); a system made for the place keeps it",
                          scaleText(plan.scale).c_str(), (hi / lo - 1.0) * 100.0,
                          crs.code.empty() ? crs.name.c_str() : crs.code.c_str());
            plan.warnings.push_back(s);
        }
    }
    plan.frameX = mm(kMargin);
    plan.frameY = mm(kMargin + kTopBand);
    plan.frameW = size.width;
    plan.frameH = size.height;

    // 6. The true north at the centre.
    plan.north = northAngle(crs, centre);
    return plan;
}

MapReport composePage(Map& map, const PageSpec& spec, const PagePlan& plan, Canvas& cv) {
    cv.clear(kWhite);

    // The map, in its frame, at the pixel ratio of paper (§74): the scale
    // and the widths of the styles both right on the paper.
    const double ratio = map.pixelRatio();
    map.setPixelRatio(kPixelPerPoint);
    cv.save();
    cv.clipRect(plan.frameX, plan.frameY, plan.frameW, plan.frameH);
    cv.translate(plan.frameX, plan.frameY);
    MapReport report = map.render(cv, plan.view);
    cv.restore();
    map.setPixelRatio(ratio);
    for (const auto& w : plan.warnings) report.warnings.push_back("page: " + w);

    // The frame.
    rect(cv, plan.frameX, plan.frameY, plan.frameW, plan.frameH);
    StrokeStyle border;
    border.color = kInk;
    border.width = 0.8;
    cv.stroke(border, PathMode::Consume);

    // The title, the subtitle and the date.
    const double right = plan.frameX + plan.frameW;
    const double titleLine = mm(kMargin + 8.5);
    text(cv, spec.title, font(16.0, true), plan.frameX, titleLine, 0.0, kInk);
    std::string aside = spec.subtitle;
    const std::string date = spec.date.empty() ? today() : spec.date;
    aside += (aside.empty() ? "" : " · ") + date;
    text(cv, aside, font(9.0), right, titleLine, 1.0, kGrey);

    // The foot: the scale bar on the left, the north arrow on the right, the small print.
    const double foot = plan.frameY + plan.frameH;
    if (spec.scaleBar) drawScaleBar(cv, plan, plan.frameX, foot + mm(6.0));
    if (spec.northArrow && plan.north) drawNorthArrow(cv, right - mm(8.0), foot + mm(14.0), *plan.north);
    if (spec.legend) {   // §74 (5b): between the scale bar (60 mm) and the north arrow (18 mm)
        const auto groups = legendEntries(map, plan.view, kPixelPerPoint, &report);
        const double x0 = plan.frameX + mm(60.0);
        const int left = drawLegend(cv, groups, x0, foot + mm(3.0), right - mm(18.0) - x0, mm(5.0 * kLegendLine));
        if (left > 0)
            report.warnings.push_back("page: legend: " + std::to_string(left) + " line" + (left > 1 ? "s" : "") +
                                      " not shown (no room: an A3, fewer layers, or legend: false on some)");
    }
    std::string small = map.crs().code.empty() ? map.crs().name : map.crs().code;
    if (!spec.generator.empty()) small += (small.empty() ? "" : " · ") + spec.generator;
    text(cv, small, font(6.5), right, plan.height - mm(kMargin) + mm(1.0), 1.0, kGrey);
    return report;
}

} // namespace r2d
