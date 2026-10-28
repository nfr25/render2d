# SPDX-License-Identifier: AGPL-3.0-or-later
"""The numbers of viewer2d notions, chapter 10, checked in code.
Run with:  python tests/test_grid.py   (or python -m pytest tests)
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from tiles2d import grid  # noqa: E402


def test_tile_side():
    assert grid.tile_side(0) == 256
    assert grid.tile_side(-2) == 1024
    assert grid.tile_side(-4) == 4096
    assert grid.tile_side(1) == 128


def test_tile_of_and_bbox():
    # Point (149 000 ; 170 000) at zoom 0: tile 0/582/-665.
    assert grid.tile_of(149000, 170000, 0) == (582, -665)
    assert grid.tile_bbox(0, 582, -665) == (148992, 169984, 149248, 170240)
    # Same point at zoom -4: tile -4/36/-42.
    assert grid.tile_of(149000, 170000, -4) == (36, -42)
    assert grid.tile_bbox(-4, 36, -42) == (147456, 167936, 151552, 172032)


def test_neighbours_touch():
    # Tiles 582 and 583 share an edge: no gap, no overlap.
    a = grid.tile_bbox(0, 582, -665)
    b = grid.tile_bbox(0, 583, -665)
    assert a[2] == b[0]
    # Row -664 is just below row -665.
    c = grid.tile_bbox(0, 582, -664)
    assert c[3] == a[1]


def test_covering():
    cols, rows = grid.tiles_covering((148992, 169984, 149248, 170240), 0)
    assert list(cols) == [582] and list(rows) == [-665]      # exactly one tile
    cols, rows = grid.tiles_covering((148992, 169984, 149249, 170240), 0)
    assert list(cols) == [582, 583]                          # one metre more


def test_metatiles():
    n = 4
    assert grid.metatile_of(582, -665, n) == (145, -167)     # floor division, also for -665
    tiles = list(grid.metatile_tiles(145, -167, n))
    assert len(tiles) == 16
    assert tiles[0] == (580, -668, 0, 0) and tiles[-1] == (583, -665, 3, 3)
    # The metatile envelope is exactly the union of its 16 tiles.
    x0, y0, x1, y1 = grid.metatile_bbox(0, 145, -167, n)
    assert (x0, y1) == grid.tile_bbox(0, 580, -668)[0::3]
    assert (x1, y0) == (grid.tile_bbox(0, 583, -665)[2], grid.tile_bbox(0, 583, -665)[1])
    assert x1 - x0 == 4 * 256 and y1 - y0 == 4 * 256
    # One tile -> one metatile. Tiles 582..583 stay in metatile 145 (tiles 580..583);
    # tiles 582..584 cross into metatile 146.
    assert grid.metatiles_covering((148992, 169984, 149248, 170240), 0, n) == [(145, -167)]
    assert grid.metatiles_covering((148992, 169984, 149504, 170240), 0, n) == [(145, -167)]
    assert grid.metatiles_covering((148992, 169984, 149760, 170240), 0, n) == [(145, -167), (146, -167)]


if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_'):
            fn()
    print('grid tests passed')
