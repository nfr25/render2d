// SPDX-License-Identifier: AGPL-3.0-or-later
// Step 1 demo: one test scene, written as PNG, SVG and PDF by the same code.
// Each shape checks a chapter of notions.html:
//   - two blocks with a courtyard, same dirty data (rings turning the same
//     way), filled even-odd (hole kept) and non-zero (hole lost)    §11
//   - two 1-pixel lines at x = 300 (blurred) and x = 310.5 (sharp)  §9
//   - a thick dashed line given in metres, width in pixels          §10, §12
//   - a cubic Bezier curve                                          §10
//
// Usage: render2d_demo [output ...]   (default: test.png test.svg test.pdf)

#include <cstdio>
#include <string>
#include <vector>

#include "backend/cairo/cairo_target.hpp"
#include "canvas/world_path.hpp"
#include "core/view.hpp"

using namespace r2d;

namespace {

// A rectangle ring turning clockwise in world coordinates (Y up),
// the shapefile convention for outer rings. notions §11.
std::vector<Point> rectRing(double xmin, double ymin, double xmax, double ymax) {
    return {{xmin, ymin}, {xmin, ymax}, {xmax, ymax}, {xmax, ymin}};
}

// One block of buildings with a courtyard. The courtyard ring turns the SAME
// way as the outer ring: "dirty" data, on purpose.
void drawBlock(Canvas& cv, const Affine& m, double x0, FillRule rule) {
    const auto outer = rectRing(x0,       160030.0, x0 + 110.0, 160170.0);
    const auto court = rectRing(x0 + 30.0, 160080.0, x0 + 80.0,  160130.0);

    addWorldPath(cv, m, outer, true);
    addWorldPath(cv, m, court, true);   // same path: one polygon, two rings

    const FillStyle   fill{.color = Color::fromHex(0xD9A066, 0.9), .rule = rule};
    const StrokeStyle edge{.color = Color::fromHex(0x5A3A1A), .width = 1.5};
    cv.fillAndStroke(fill, edge);
}

void drawScene(Canvas& cv, const View& view) {
    const Affine& m = view.worldToImage;
    const double  w = view.size.width;
    const double  h = view.size.height;

    cv.clear(Color::fromHex(0xFFFFFF));

    // §11: left even-odd, right non-zero, identical data.
    drawBlock(cv, m, 150020.0, FillRule::EvenOdd);
    drawBlock(cv, m, 150170.0, FillRule::NonZero);

    // §9: in pixel coordinates directly. x = 300 -> two grey columns,
    // x = 310.5 -> one black column. Zoom into the PNG to see it.
    const StrokeStyle hair{.color = Color::fromHex(0x000000), .width = 1.0};
    cv.moveTo({300.0, 60.0});  cv.lineTo({300.0, 340.0});  cv.stroke(hair, PathMode::Consume);
    cv.moveTo({310.5, 60.0});  cv.lineTo({310.5, 340.0});  cv.stroke(hair, PathMode::Consume);

    // §10, §12: vertices in metres, width and dashes in pixels.
    const std::vector<Point> track = {
        {150020.0, 160012.0}, {150080.0, 160022.0}, {150140.0, 160008.0},
        {150200.0, 160022.0}, {150260.0, 160010.0}};
    const StrokeStyle dashed{.color  = Color::fromHex(0xC0501F, 0.85),
                             .width  = 5.0,
                             .cap    = LineCap::Round,
                             .join   = LineJoin::Round,
                             .dashes = {16.0, 8.0}};
    addWorldPath(cv, m, track, false);
    cv.stroke(dashed, PathMode::Consume);

    // §10: a cubic Bezier, in pixels.
    cv.moveTo({40.0, 40.0});
    cv.curveTo({200.0, -20.0}, {440.0, 100.0}, {600.0, 30.0});
    cv.stroke(StrokeStyle{.color = Color::fromHex(0x2B6C8F), .width = 2.5, .cap = LineCap::Round},
              PathMode::Consume);

    // Frame on half-pixels: crisp 1-pixel border. §9.
    cv.moveTo({0.5, 0.5});
    cv.lineTo({w - 0.5, 0.5});
    cv.lineTo({w - 0.5, h - 0.5});
    cv.lineTo({0.5, h - 0.5});
    cv.closePath();
    cv.stroke(StrokeStyle{.color = Color::fromHex(0x22252B), .width = 1.0}, PathMode::Consume);
}

const char* formatName(OutputFormat f) {
    switch (f) {
        case OutputFormat::Png: return "PNG";
        case OutputFormat::Svg: return "SVG";
        case OutputFormat::Pdf: return "PDF";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> outputs(argv + 1, argv + argc);
    if (outputs.empty()) outputs = {"test.png", "test.svg", "test.pdf"};

    // 320 m x 200 m of Lambert 72 on 640 x 400: 0.5 m per pixel.
    const auto view = makeView({150000.0, 160000.0, 150320.0, 160200.0}, {640, 400});
    if (!view) {
        std::fprintf(stderr, "invalid view\n");
        return 1;
    }

    int failures = 0;
    for (const std::string& path : outputs) {
        std::string error;
        auto target = CairoTarget::create(path, view->size, error);
        if (!target) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
            ++failures;
            continue;
        }
        drawScene(target->canvas(), *view);
        if (!target->finish(error)) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
            ++failures;
            continue;
        }
        std::printf("wrote %s (%s, %dx%d, %.2f m/px, scale 1:%.0f)\n", path.c_str(),
                    formatName(target->format()), view->size.width, view->size.height,
                    view->resolution, view->scaleDenominator());
    }
    return failures == 0 ? 0 : 1;
}
