// SPDX-License-Identifier: AGPL-3.0-or-later
// The numbers of notions.html, checked in code.
// Running example: the 5000 m square 150000..155000 x 160000..165000
// on a 1000 x 1000 image, and the point (152500, 162500).

#include <cfloat>
#include <cmath>
#include <cstdio>

#include "check.hpp"
#include "core/affine.hpp"
#include "core/view.hpp"

using namespace r2d;

static constexpr double kPx  = 1e-9; // tolerance in pixels
static constexpr double kM   = 1e-6; // tolerance in metres

static const Envelope kSquare{150000.0, 160000.0, 155000.0, 165000.0};

// notions §3: by hand, then §5: the same in matrix form.
static void testForward() {
    const View v = *makeView(kSquare, {1000, 1000});
    CHECK_NEAR(v.resolution, 5.0, 1e-12);
    CHECK(!v.widened);

    const Affine& m = v.worldToImage;
    CHECK_NEAR(m.a,  0.2,     1e-12);
    CHECK_NEAR(m.d, -0.2,     1e-12);
    CHECK_NEAR(m.e, -30000.0, 1e-6);
    CHECK_NEAR(m.f,  33000.0, 1e-6);

    const Point p = m.apply({152500.0, 162500.0});
    CHECK_NEAR(p.x, 500.0, kPx);
    CHECK_NEAR(p.y, 500.0, kPx);

    // Corners: top-left of the terrain is pixel (0,0), bottom-right is (1000,1000).
    const Point tl = m.apply({150000.0, 165000.0});
    const Point br = m.apply({155000.0, 160000.0});
    CHECK_NEAR(tl.x, 0.0, kPx);    CHECK_NEAR(tl.y, 0.0, kPx);
    CHECK_NEAR(br.x, 1000.0, kPx); CHECK_NEAR(br.y, 1000.0, kPx);
}

// notions §6: determinant and inverse.
static void testInverse() {
    const View v = *makeView(kSquare, {1000, 1000});
    CHECK_NEAR(v.worldToImage.determinant(), -0.04, 1e-15);

    const Point w = v.imageToWorld.apply({500.0, 500.0});
    CHECK_NEAR(w.x, 152500.0, kM);
    CHECK_NEAR(w.y, 162500.0, kM);

    // Center of the first pixel.
    const Point c = v.imageToWorld.apply({0.5, 0.5});
    CHECK_NEAR(c.x, 150002.5, kM);
    CHECK_NEAR(c.y, 164997.5, kM);

    // Inverse coefficients: X = 5x + 150000, Y = -5y + 165000.
    const Affine& i = v.imageToWorld;
    CHECK_NEAR(i.a,  5.0, 1e-9);
    CHECK_NEAR(i.d, -5.0, 1e-9);
    CHECK_NEAR(i.e, 150000.0, kM);
    CHECK_NEAR(i.f, 165000.0, kM);

    // Not invertible: everything squashed onto a line.
    CHECK(!Affine::scaling(0.0, 1.0).inverse().has_value());
}

// notions §5: composition with a x2 "high density screen" scaling.
static void testComposition() {
    const View   v  = *makeView(kSquare, {1000, 1000});
    const Affine m2 = Affine::scaling(2.0, 2.0) * v.worldToImage;
    CHECK_NEAR(m2.a,  0.4,     1e-12);
    CHECK_NEAR(m2.e, -60000.0, 1e-6);
    CHECK_NEAR(m2.f,  66000.0, 1e-6);

    const Point p = m2.apply({152500.0, 162500.0});
    CHECK_NEAR(p.x, 1000.0, kPx);
    CHECK_NEAR(p.y, 1000.0, kPx);

    // Order matters: translate-then-scale differs from scale-then-translate.
    const Affine ts = Affine::scaling(2.0, 2.0) * Affine::translation(10.0, 0.0);
    const Affine st = Affine::translation(10.0, 0.0) * Affine::scaling(2.0, 2.0);
    CHECK_NEAR(ts.apply({0.0, 0.0}).x, 20.0, 1e-12);
    CHECK_NEAR(st.apply({0.0, 0.0}).x, 10.0, 1e-12);

    // m * m^-1 is the identity.
    const Affine id = v.worldToImage * *v.worldToImage.inverse();
    CHECK_NEAR(id.a, 1.0, 1e-12); CHECK_NEAR(id.b, 0.0, 1e-12);
    CHECK_NEAR(id.c, 0.0, 1e-12); CHECK_NEAR(id.d, 1.0, 1e-12);
    CHECK_NEAR(id.e, 0.0, 1e-9);  CHECK_NEAR(id.f, 0.0, 1e-9);
}

// notions §4: same terrain on 1000 x 600 -> widened, pixels stay square.
static void testAspect() {
    const View v = *makeView(kSquare, {1000, 600});
    CHECK(v.widened);
    CHECK_NEAR(v.resolution, 5000.0 / 600.0, 1e-12);
    CHECK_NEAR(v.shown.xmin, 152500.0 - 1000.0 * (5000.0 / 600.0) / 2.0, kM); // ~148333.33
    CHECK_NEAR(v.shown.ymax, 165000.0, kM);
    CHECK_NEAR(v.shown.width(), 1000.0 * 5000.0 / 600.0, kM);                 // ~8333.33

    const Point p = v.worldToImage.apply({152500.0, 162500.0});
    CHECK_NEAR(p.x, 500.0, 1e-7);
    CHECK_NEAR(p.y, 300.0, 1e-7);
}

// notions §7: scale denominator with the 0.28 mm pixel.
static void testScale() {
    const View v = *makeView(kSquare, {1000, 1000});
    CHECK_NEAR(v.scaleDenominator(), 5.0 / 0.00028, 1e-6); // 17857.14...
    CHECK(std::lround(v.scaleDenominator()) == 17857);
}

// A sub-pixel difference (rounded height, bbox from a browser) is not "widened".
static void testWidenedTolerance() {
    // 5000 m x 3000 m: exact height for 1000 px wide would be 600 px.
    CHECK(!makeView({0.0, 0.0, 5000.0, 3000.0}, {1000, 600}).value().widened);
    // Height slightly off (600.001 px worth of terrain): still not widened.
    CHECK(!makeView({0.0, 0.0, 5000.0, 3000.005}, {1000, 600}).value().widened);
    // One full pixel more terrain in height than the image allows: widened.
    CHECK(makeView({0.0, 0.0, 5000.0, 3005.0}, {1000, 600}).value().widened);
}

// Invalid input gives no view instead of garbage.
static void testInvalid() {
    CHECK(!makeView({150000.0, 160000.0, 150000.0, 165000.0}, {1000, 1000}).has_value()); // zero width
    CHECK(!makeView(kSquare, {0, 1000}).has_value());
    CHECK(!makeView({0.0, 0.0, NAN, 1.0}, {10, 10}).has_value());
}

// notions §8: why double. Documents float spacing at Lambert and Mercator values.
static void testFloatSpacing() {
    const float lambert  = 150000.0f;
    const float mercator = 6500000.0f;
    CHECK_NEAR(std::nextafter(lambert,  FLT_MAX) - lambert,  1.0 / 64.0, 0.0); // 1.6 cm
    CHECK_NEAR(std::nextafter(mercator, FLT_MAX) - mercator, 0.5,        0.0); // 50 cm
    CHECK_NEAR(static_cast<double>(static_cast<float>(6500000.3)), 6500000.5, 0.0);

    // In double, the same Mercator value keeps sub-nanometre spacing.
    const double md = 6500000.0;
    CHECK(std::nextafter(md, DBL_MAX) - md < 1e-9);
}

// Round trip on a few arbitrary points: world -> image -> world.
static void testRoundTrip() {
    const View v = *makeView({148213.7, 161002.3, 153977.1, 164519.9}, {1280, 768});
    const Point pts[] = {{148213.7, 161002.3}, {150000.0, 163000.0}, {153977.1, 164519.9}, {151234.567, 162345.678}};
    for (const Point& w : pts) {
        const Point back = v.imageToWorld.apply(v.worldToImage.apply(w));
        CHECK_NEAR(back.x, w.x, kM);
        CHECK_NEAR(back.y, w.y, kM);
    }
}

int main() {
    testForward();
    testInverse();
    testComposition();
    testAspect();
    testScale();
    testWidenedTolerance();
    testInvalid();
    testFloatSpacing();
    testRoundTrip();
    return check::summary();
}
