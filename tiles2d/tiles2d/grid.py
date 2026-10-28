# SPDX-License-Identifier: AGPL-3.0-or-later
"""The tile grid: z/x/y <-> envelope in map units (Lambert metres).

Same convention as Leaflet's L.CRS.Simple, so the pyramids can be shown by a
plain L.tileLayer (viewer2d notions, chapter 10):

    pixel = (X * 2^z, -Y * 2^z)      (1 metre = 1 pixel at zoom 0, Y flipped)
    tile  = floor(pixel / 256)

The single place where this is computed: viewer2d imports it too, so the two
can never disagree by a pixel.
"""
from __future__ import annotations

import math
from collections.abc import Iterator

TILE_SIZE = 256   # pixels

Envelope = tuple[float, float, float, float]   # xmin, ymin, xmax, ymax


def tile_side(z: int) -> float:
    """Side of one tile in metres: 256 * 2^-z (256 m at zoom 0, 4096 m at zoom -4)."""
    return TILE_SIZE * 2.0 ** -z


def tile_of(x: float, y: float, z: int) -> tuple[int, int]:
    """The tile containing the point (x, y). Rows are negative for positive Y."""
    side = tile_side(z)
    return math.floor(x / side), math.floor(-y / side)


def tile_bbox(z: int, tx: int, ty: int) -> Envelope:
    """Envelope of tile z/tx/ty. Tile row ty covers pixel rows ty*256 .. (ty+1)*256,
    i.e. Y from -(ty+1)*side to -ty*side."""
    side = tile_side(z)
    return tx * side, -(ty + 1) * side, (tx + 1) * side, -ty * side


def tiles_covering(bbox: Envelope, z: int) -> tuple[range, range]:
    """Columns and rows of the tiles touching bbox. A tile that only touches the
    border (zero-width overlap) is not included."""
    xmin, ymin, xmax, ymax = bbox
    side = tile_side(z)
    cols = range(math.floor(xmin / side), math.ceil(xmax / side))
    rows = range(math.floor(-ymax / side), math.ceil(-ymin / side))
    return cols, rows


# --- metatiles: blocks of n x n tiles rendered in one image ---------------------

def metatile_of(tx: int, ty: int, n: int) -> tuple[int, int]:
    """The metatile containing tile (tx, ty). Floor division, also for negative rows."""
    return tx // n, ty // n


def metatile_tiles(mx: int, my: int, n: int) -> Iterator[tuple[int, int, int, int]]:
    """(tx, ty, i, j) for each tile of metatile (mx, my): i = column, j = row in the block."""
    for j in range(n):
        for i in range(n):
            yield mx * n + i, my * n + j, i, j


def metatile_bbox(z: int, mx: int, my: int, n: int) -> Envelope:
    """Envelope of a metatile: from its top-left tile to its bottom-right tile."""
    x0, _, _, y1 = tile_bbox(z, mx * n, my * n)
    _, y0, x1, _ = tile_bbox(z, mx * n + n - 1, my * n + n - 1)
    return x0, y0, x1, y1


def metatiles_covering(bbox: Envelope, z: int, n: int) -> list[tuple[int, int]]:
    cols, rows = tiles_covering(bbox, z)
    if not cols or not rows:
        return []
    mx0, my0 = metatile_of(cols.start, rows.start, n)
    mx1, my1 = metatile_of(cols.stop - 1, rows.stop - 1, n)
    return [(mx, my) for my in range(my0, my1 + 1) for mx in range(mx0, mx1 + 1)]
