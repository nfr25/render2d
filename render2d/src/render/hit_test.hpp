// SPDX-License-Identifier: AGPL-3.0-or-later
// Does a point touch a geometry? Pure geometry, in map units (metres).
// See notions §52.
#pragma once

#include "core/geometry.hpp"
#include "core/point.hpp"

namespace r2d {

// Distance from p to the segment [a, b]: the projection of p on the line ab,
// brought back between a and b. §52, "une ligne".
double distanceToSegment(Point p, Point a, Point b);

// Distance from p to the nearest edge of the geometry's parts. `closed`: each
// part is a ring (its last point joins its first), as for a polygon's outline.
double distanceToParts(const Geometry& g, Point p, bool closed);

// Distance from p to the nearest point of a (multi)point.
double distanceToPoints(const Geometry& g, Point p);

// The even-odd rule (§11): a half-line from p to the right crosses an odd
// number of edges, all rings together. A courtyard is outside. §52.
bool insideEvenOdd(const Geometry& g, Point p);

// ---- a zone (§54, §55) ------------------------------------------------------

// Standing at a, looking at b: > 0 if c is on the left, < 0 on the right,
// 0 on the line ab. The cross product AB x AC. §55.
double orient(Point a, Point b, Point c);

// Do the segments [a, b] and [c, d] share a point? Touching counts (§55).
bool segmentsCross(Point a, Point b, Point c, Point d);

// Does the geometry share a point with the zone (`zone`: one ring, closed or
// not, read with the even-odd rule)? The three cases of §54: a vertex in the
// zone, an edge crossing an edge of the zone, the zone inside a polygon.
bool touchesArea(const Geometry& g, const Geometry& zone);

} // namespace r2d
