// SPDX-License-Identifier: AGPL-3.0-or-later
// Touching a geometry: the numbers of notions §52, then a whole layer through
// queryLayer: what the style draws is what is touched (§51).

#include <cmath>
#include <utility>
#include <variant>
#include <vector>

#include "check.hpp"
#include "render/hit_test.hpp"
#include "render/query_layer.hpp"

using namespace r2d;

namespace {

Geometry polygon(std::vector<std::vector<Point>> rings) {
    Geometry g;
    g.type = GeomType::Polygon;
    for (const auto& r : rings) {
        const std::size_t start = g.points.size();
        g.points.insert(g.points.end(), r.begin(), r.end());
        g.closePart(start);
    }
    return g;
}

// §52, "un point": a porch 3 m east and 2 m north, threshold 3.875 m.
void testPoint() {
    Geometry door;
    door.type   = GeomType::Point;
    door.points = {{0.0, 0.0}};
    door.parts  = {{0, 1}};
    const double d = distanceToPoints(door, {3.0, 2.0});
    CHECK_NEAR(d, std::sqrt(13.0), 1e-12);        // 3.61 m
    CHECK(d <= (3.75 + 4.0) * 0.5);               // 7.75 px x 0.5 m/px = 3.875 m
}

// §52, "une ligne": A (0;0), B (10;0), P (4;1.2): t = 0.4, distance 1.2 m.
void testSegment() {
    CHECK_NEAR(distanceToSegment({4.0, 1.2}, {0.0, 0.0}, {10.0, 0.0}), 1.2, 1e-12);
    CHECK(distanceToSegment({4.0, 1.2}, {0.0, 0.0}, {10.0, 0.0}) <= (4.0 + 1.5) * 0.25);   // 1.375 m
    // Beyond B (t > 1): the distance to B itself.
    CHECK_NEAR(distanceToSegment({13.0, 4.0}, {0.0, 0.0}, {10.0, 0.0}), 5.0, 1e-12);
}

// §52, "un polygone": a 10 m square with a courtyard from (4;4) to (6;6).
void testEvenOdd() {
    const Geometry b = polygon({{{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}},
                                {{4, 4}, {6, 4}, {6, 6}, {4, 6}, {4, 4}}});
    CHECK(insideEvenOdd(b, {2.0, 5.0}));     // 3 crossings: inside
    CHECK(!insideEvenOdd(b, {5.0, 5.0}));    // 2 crossings: the courtyard
    CHECK(!insideEvenOdd(b, {12.0, 5.0}));   // 0 crossings: outside
    // From the courtyard, the nearest wall is 1 m away: within 4 px x 0.25 m/px = 1 m.
    CHECK_NEAR(distanceToParts(b, {5.0, 5.0}, true), 1.0, 1e-12);
    // A half-line through a vertex (y = 4 at x = 6, the courtyard's corner) counts once.
    CHECK(insideEvenOdd(b, {8.0, 4.0}));
}

// An in-memory layer: a building with a courtyard (AREA 96) and a shed (AREA 20).
class TwoBuildings final : public FeatureSource {
public:
    TwoBuildings() {
        geoms_.push_back(polygon({{{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}},
                                  {{4, 4}, {6, 4}, {6, 6}, {4, 6}, {4, 4}}}));
        geoms_.push_back(polygon({{{20, 0}, {24, 0}, {24, 5}, {20, 5}, {20, 0}}}));
        fields_ = {{"AREA", FieldType::Real}};
        rows_   = {{96.0}, {20.0}};
    }
    const std::vector<FieldDef>& fields() const override { return fields_; }
    void setNeededFields(const std::vector<int>&) override {}
    void setSpatialFilter(const Envelope& e) override { filter_ = e; }
    ReadStats forEach(const FeatureVisitor& visit) override {
        ReadStats s;
        for (std::size_t i = 0; i < geoms_.size(); ++i) {
            const Envelope e = geoms_[i].envelope();
            if (e.xmax < filter_.xmin || e.xmin > filter_.xmax || e.ymax < filter_.ymin || e.ymin > filter_.ymax)
                continue;   // as GDAL: only the features whose rectangle touches the filter
            ++s.read; ++s.drawn;
            visit(geoms_[i], rows_[i], static_cast<FeatureId>(100 + i));
        }
        return s;
    }
private:
    std::vector<Geometry>   geoms_;
    std::vector<FieldDef>   fields_;
    std::vector<Attributes> rows_;
    Envelope                filter_{-1e9, -1e9, 1e9, 1e9};
};

Style fillStyle(std::optional<Expression> filter) {
    Rule r;
    r.filter = std::move(filter);
    // Built in place in the vector: copying a temporary symbolizer into the
    // variant makes GCC 16 (-O2) see fields "maybe uninitialized" that are not.
    r.symbolizers.emplace_back(std::in_place_type<PolygonSymbolizer>);
    std::get<PolygonSymbolizer>(r.symbolizers.back()).fill = FillStyle{.color = Color::fromHex(0xC98B6B)};
    Style s;
    s.featureStyles.push_back({{r}});
    return s;
}

// §51: one selects only what one sees.
void testQueryLayer() {
    const View view = *makeView({-5.0, -5.0, 35.0, 15.0}, {160, 80});   // 0.25 m/px
    TwoBuildings layer;

    const Style all = fillStyle(std::nullopt);
    const StyleByType byType{&all, &all, &all};
    auto hits = queryLayer(layer, view, {2.0, 5.0}, 4.0, byType);
    CHECK(hits.size() == 1 && hits[0].fid == 100);                       // inside the building
    CHECK(hits.size() == 1 && std::get<double>(hits[0].attributes[0]) == 96.0);
    CHECK(queryLayer(layer, view, {5.0, 5.0}, 4.0, byType).size() == 1);   // courtyard, but the wall is 1 m away
    CHECK(queryLayer(layer, view, {5.0, 5.0}, 2.0, byType).empty());       // 2 px = 0.5 m: too far from the wall
    CHECK(queryLayer(layer, view, {22.0, 2.0}, 4.0, byType).size() == 1);  // the shed

    // A style that only draws AREA >= 30: the shed is not drawn, not selectable.
    std::string err;
    const auto expr = Expression::parseCondition("AREA >= 30", err);
    CHECK(expr.has_value());
    if (!expr) return;
    const Style big = fillStyle(*expr);
    const StyleByType bigByType{&big, &big, &big};
    CHECK(queryLayer(layer, view, {22.0, 2.0}, 4.0, bigByType).empty());
    CHECK(queryLayer(layer, view, {2.0, 5.0}, 4.0, bigByType).size() == 1);
}

// §55: the numbers of "à gauche ou à droite".
void testOrient() {
    const Point A{0, 0}, B{10, 0};
    CHECK_NEAR(orient(A, B, {4, -3}), -30.0, 1e-12);
    CHECK_NEAR(orient(A, B, {6, 5}), 50.0, 1e-12);
    CHECK_NEAR(orient({4, -3}, {6, 5}, A), 38.0, 1e-12);
    CHECK_NEAR(orient({4, -3}, {6, 5}, B), -42.0, 1e-12);
    CHECK(segmentsCross(A, B, {4, -3}, {6, 5}));      // first example: they cross
    CHECK_NEAR(orient({12, -3}, {14, 5}, A), 102.0, 1e-12);
    CHECK_NEAR(orient({12, -3}, {14, 5}, B), 22.0, 1e-12);
    CHECK(!segmentsCross(A, B, {12, -3}, {14, 5}));   // second: beyond B
    CHECK(segmentsCross(A, B, {10, 0}, {10, 5}));     // touching at B counts
    CHECK(segmentsCross(A, B, {5, 0}, {15, 0}));      // overlapping on one line
    CHECK(!segmentsCross(A, B, {11, 0}, {15, 0}));    // on the line, but after B
}

Geometry ring(std::vector<Point> pts) {
    Geometry z;
    z.type   = GeomType::Polygon;
    z.points = std::move(pts);
    z.closePart(0);
    return z;
}

// §54: the three cases, and the courtyard.
void testTouchesArea() {
    const Geometry b = polygon({{{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}},
                                {{4, 4}, {6, 4}, {6, 6}, {4, 6}, {4, 4}}});
    CHECK(touchesArea(b, ring({{8, 8}, {12, 8}, {12, 12}, {8, 12}})));     // 1. a vertex (10;10) inside
    CHECK(touchesArea(b, ring({{-2, 2}, {12, 2}, {12, 3}, {-2, 3}})));     // 2. edges cross, no vertex inside
    CHECK(touchesArea(b, ring({{1, 1}, {2, 1}, {2, 2}, {1, 2}})));         // 3. the zone inside the building
    CHECK(!touchesArea(b, ring({{4.5, 4.5}, {5.5, 4.5}, {5.5, 5.5}, {4.5, 5.5}})));   // in the courtyard
    CHECK(!touchesArea(b, ring({{20, 20}, {30, 20}, {30, 30}, {20, 30}})));            // far away

    Geometry street;   // a line through a small rectangle, no vertex inside
    street.type   = GeomType::Line;
    street.points = {{-100, 5}, {100, 5}};
    street.parts  = {{0, 2}};
    CHECK(touchesArea(street, ring({{0, 0}, {1, 0}, {1, 10}, {0, 10}})));
    CHECK(!touchesArea(street, ring({{0, 6}, {1, 6}, {1, 10}, {0, 10}})));

    Geometry tree;
    tree.type   = GeomType::Point;
    tree.points = {{3, 3}};
    tree.parts  = {{0, 1}};
    CHECK(touchesArea(tree, ring({{0, 0}, {5, 0}, {5, 5}, {0, 5}})));
    // A bow-tie (§54): both wings are inside, the crossing point too.
    CHECK(touchesArea(tree, ring({{0, 0}, {6, 6}, {6, 0}, {0, 6}})));
    tree.points = {{3, 5.5}};   // above the crossing, between the wings: outside
    CHECK(!touchesArea(tree, ring({{0, 0}, {6, 6}, {6, 0}, {0, 6}})));
}

// §54 through a layer: what is drawn, and the cap.
void testQueryLayerArea() {
    const View view = *makeView({-5.0, -5.0, 35.0, 15.0}, {160, 80});
    TwoBuildings layer;
    const Style all = fillStyle(std::nullopt);
    const StyleByType byType{&all, &all, &all};
    const Geometry both = ring({{-1, -1}, {30, -1}, {30, 12}, {-1, 12}});

    bool truncated = false;
    auto hits = queryLayerArea(layer, view, both, byType, kNoLimit, truncated);
    CHECK(hits.size() == 2 && !truncated);
    CHECK(hits.size() == 2 && hits[0].fid == 101);   // the last drawn first

    truncated = false;
    hits = queryLayerArea(layer, view, both, byType, 1, truncated);
    CHECK(hits.size() == 1 && truncated);

    std::string err;
    const auto expr = Expression::parseCondition("AREA >= 30", err);
    if (!expr) return;
    const Style big = fillStyle(*expr);
    const StyleByType bigByType{&big, &big, &big};
    truncated = false;
    hits = queryLayerArea(layer, view, both, bigByType, kNoLimit, truncated);
    CHECK(hits.size() == 1 && hits[0].fid == 100);   // the shed is not drawn: not taken
}

} // namespace

int main() {
    testPoint();
    testSegment();
    testEvenOdd();
    testQueryLayer();
    testOrient();
    testTouchesArea();
    testQueryLayerArea();
    return check::summary();
}
