// SPDX-License-Identifier: AGPL-3.0-or-later
// Writing YSLD (notions §59): the round trip. Every style of styles/ is read
// (A), written, read again (B): A and B must be equal, field by field, and
// draw the same pixels.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <variant>

#include "backend/cairo/cairo_image.hpp"
#include "check.hpp"
#include "map/map.hpp"
#include "style/ysld_reader.hpp"
#include "style/ysld_writer.hpp"

using namespace r2d;
namespace fs = std::filesystem;

namespace {

// Every field of the model, defaults included: what the comparison sees.
// (The writer omits defaults; this dump never does.)
struct Dump {
    std::ostringstream o;
    void c(const char* k, const Color& x) { o << k << '=' << x.r << ',' << x.g << ',' << x.b << ',' << x.a << ' '; }
    void d(const char* k, double x) { o << k << '=' << x << ' '; }
    void s(const char* k, const std::string& x) { o << k << "='" << x << "' "; }
    void e(const char* k, const std::optional<Expression>& x) {
        o << k << '=' << (x ? x->text() + (x->isLiteralText() ? "(lit)" : "") : "none") << ' ';
    }
    void stroke(const char* k, const std::optional<StrokeStyle>& x) {
        o << k << '{';
        if (x) {
            c("color", x->color); d("w", x->width);
            d("cap", static_cast<int>(x->cap)); d("join", static_cast<int>(x->join));
            for (double v : x->dashes) d("dash", v);
            d("off", x->dashOffset);
        }
        o << "} ";
    }
    void fill(const char* k, const std::optional<FillStyle>& x) {
        o << k << '{';
        if (x) { c("color", x->color); d("rule", static_cast<int>(x->rule)); }
        o << "} ";
    }
    void operator()(const PolygonSymbolizer& p) {
        o << "polygon ";
        fill("fill", p.fill); stroke("stroke", p.stroke);
        if (p.graphicFill) {
            d("gsize", p.graphicFill->size); d("gshape", static_cast<int>(p.graphicFill->shape));
            fill("gfill", p.graphicFill->fill); stroke("gstroke", p.graphicFill->stroke);
        }
    }
    void operator()(const LineSymbolizer& l) { o << "line "; stroke("stroke", l.stroke); }
    void operator()(const PointSymbolizer& p) {
        o << "point "; d("size", p.size); fill("fill", p.fill); stroke("stroke", p.stroke);
        d("shape", static_cast<int>(p.shape)); s("image", fs::path(p.image).lexically_normal().generic_string());
        e("sizeExpr", p.sizeExpr); e("rot", p.rotation); d("metres", p.metres);
    }
    void operator()(const TextSymbolizer& t) {
        o << "text "; e("label", t.label); s("font", t.font.family); d("fs", t.font.size);
        d("bold", t.font.bold); d("it", t.font.italic); c("fill", t.fill);
        if (t.halo) { d("hr", t.halo->radius); c("hc", t.halo->color); }
        d("ax", t.anchorX); d("ay", t.anchorY); d("dx", t.displacementX); d("dy", t.displacementY);
        e("rot", t.rotation);
    }
    void operator()(const RasterSymbolizer& r) { o << "raster "; d("opacity", r.opacity); }
    std::string style(const Style& st) {
        s("name", st.name);
        for (const auto& f : st.featureStyles) {
            o << "\nPASS";
            for (const auto& r : f.rules) {
                o << "\n RULE "; s("name", r.name); d("min", r.scale.min); d("max", r.scale.max);
                e("filter", r.filter); d("else", r.isElse); d("invalid", r.invalid); s("bad", r.invalidFilter);
                for (const auto& sym : r.symbolizers) { o << "\n  "; std::visit(*this, sym); }
            }
        }
        return o.str();
    }
};

std::string dump(const Style& s) { return Dump{}.style(s); }

const fs::path kStyles = fs::path(R2D_SOURCE_DIR) / "styles";

void testRoundTrip() {
    int files = 0;
    for (const auto& entry : fs::directory_iterator(kStyles)) {
        if (entry.path().extension() != ".yaml") continue;
        ++files;
        const std::string dir = entry.path().parent_path().string();
        const YsldResult a = readYsldFile(entry.path().string());
        CHECK(a.style.has_value());
        if (!a.style) continue;
        const std::string text = writeYsld(*a.style, dir);
        const YsldResult b = parseYsld(text, dir);
        CHECK(b.style.has_value());
        if (!b.style) continue;
        const bool same = dump(*a.style) == dump(*b.style);
        CHECK(same);
        if (!same)
            std::printf("     %s\n---- A\n%s\n---- B\n%s\n---- text\n%s\n", entry.path().filename().string().c_str(),
                        dump(*a.style).c_str(), dump(*b.style).c_str(), text.c_str());
        CHECK(writeYsld(*b.style, dir) == text);   // writing again changes nothing
        CHECK(b.warnings.empty() || !a.warnings.empty());   // the writer adds no warning
    }
    CHECK(files >= 10);   // every style of the project
}

// Things a hand-written file may hold that the writer must not lose.
void testCorners() {
    const char* src = R"(name: corners
feature-styles:
- rules:
  - name: bad filter
    filter: ${AREA >>> 3}
    symbolizers:
    - line: {stroke-color: '#112233', stroke-dasharray: '8 4', stroke-opacity: 0.5}
  - else: true
    scale: [1000, max]
    symbolizers:
    - text: {label: Evere, font-size: 12, halo: {radius: 2}}
    - text: {label: '${POLICENUM}', anchor: [0.5, 0.5], rotation: '${360 - CARTOANGLE}'}
    - point: {size: '${4 * sqrt(N)}', uom: metre, symbols: [{mark: {shape: star, fill-color: '#FF0000'}}]}
)";
    const YsldResult a = parseYsld(src);
    CHECK(a.style.has_value());
    if (!a.style) return;
    const std::string text = writeYsld(*a.style);
    const YsldResult b = parseYsld(text);
    CHECK(b.style.has_value() && dump(*a.style) == dump(*b.style));
    CHECK(text.find("${AREA >>> 3}") != std::string::npos);   // an unreadable filter is kept as written
    CHECK(text.find("label: 'Evere'") != std::string::npos);  // a fixed text stays a fixed text
    CHECK(text.find("stroke-width") == std::string::npos);    // the default width is not written
}

// §59: Map::setLayerStyle, and the same pixels with A and B.
void testSetLayerStyle() {
    const fs::path dir = fs::temp_directory_path() / "r2d_writer_test";
    fs::create_directories(dir);
    const std::string data = (dir / "squares.geojson").string();
    std::ofstream(data) << R"({"type":"FeatureCollection","features":[
      {"type":"Feature","properties":{"AREA":40},"geometry":{"type":"Polygon","coordinates":[
        [[150020,160020],[150080,160020],[150080,160080],[150020,160080],[150020,160020]]]}}]})";
    const std::string style = (kStyles / "parcelles-hachures.yaml").string();
    Map a = Map::open({{data, style}});
    Map b = Map::open({{data}});   // default style, then the written text
    const YsldResult r = readYsldFile(style);
    std::vector<std::string> warnings;
    CHECK(r.style && b.setLayerStyle(0, writeYsld(*r.style), warnings));
    const View v = *makeView({150000, 160000, 150100, 160100}, {100, 100});
    std::string err;
    auto ia = CairoImage::create({100, 100}, err), ib = CairoImage::create({100, 100}, err);
    ia->canvas().clear(Color::fromHex(0xFFFFFF));
    ib->canvas().clear(Color::fromHex(0xFFFFFF));
    a.render(ia->canvas(), v);
    b.render(ib->canvas(), v);
    bool same = true;
    for (int y = 0; y < 100; ++y)
        same = same && std::memcmp(ia->pixels() + y * ia->stride(), ib->pixels() + y * ib->stride(), 400) == 0;
    CHECK(same);

    // Unreadable text: false, the style in place.
    warnings.clear();
    CHECK(!b.setLayerStyle(0, "[not a map", warnings) && !warnings.empty());
}

} // namespace

int main() {
    testRoundTrip();
    testCorners();
    testSetLayerStyle();
    return check::summary();
}
