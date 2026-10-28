# tiles2d

Builds tile pyramids with [render2d](../render2d), the way `gdal2tiles.py` is
built on top of GDAL: tiles2d computes the tile grid and the cache, render2d
draws. render2d knows nothing about tiles.

The concepts (grid, z/x/y, pyramid, cache key, metatiles) are explained with
numbers in chapter 10 of [`viewer2d/docs/notions.en.html`](../viewer2d/docs/notions.en.html)
(in French).

## Running it

Python 3.10 or later, then:

```
pip install -r requirements.txt
python -m tiles2d --config ../viewer2d/viewer.yaml --zmin -4 --zmax 0 --out cache
```

The map file has the same format as viewer2d's `viewer.yaml` (render2d path,
layers with their styles, `start.bbox`): one file describes a map for both
tools. Then open `cache/index.html` in a browser to look at the pyramids
(no server needed), with a layer switch and a debug grid showing each tile's
border and z/x/y.

| Option | Meaning |
|---|---|
| `--zmin`, `--zmax` | Zoom levels. 1 map unit = 1 px at zoom 0; a tile covers 256 × 2^-z m (256 m at 0, 4096 m at -4). Negative zooms are normal. |
| `--bbox XMIN YMIN XMAX YMAX` | Area to tile, in map units. Default: `start.bbox` of the map file. |
| `--out DIR` | Output folder (default `cache`). |
| `--layer NAME` | Only this layer (repeatable). Names come from the source file names. |
| `--metatile N` | Tiles per metatile side (default 4: one render2d call draws 16 tiles). |
| `--jobs N` | render2d processes at the same time (default: number of cores). |
| `--force` | Redraw tiles already in the cache. |

## What it writes

```
cache/
  index.html                         preview page (Leaflet, L.CRS.Simple)
  parcelles/310bfd8b8a89/            one pyramid per layer, in a folder named by its cache key
    tiles.json                       source, style, key, zooms, bbox, creation date
    -4/36/-42.png                    z/x/y.png; rows are negative for positive Y (Leaflet's convention)
    ...
  batiments/7d54a8ccb93b/
```

- **One pyramid per layer**: layers can be switched on and off without
  redrawing, and editing one style does not invalidate the other layers.
- **The cache key** is a fingerprint of the style file content, the data files
  (size and date, all shapefile companions), and the render2d executable.
  Edit a style: the key changes and new tiles go to a new folder. Old folders
  can be deleted by hand.
- **Resuming**: metatiles whose 16 tiles already exist are skipped, so an
  interrupted run can simply be started again.
- **Whole metatiles are written**, including the tiles that stick out of the
  requested area, and fully transparent tiles too (so a server never has to
  recompute them).
- Tiles are written to a temporary name, then renamed: a server reading the
  cache at the same time never sees half a tile.

## Tests

```
python tests/test_grid.py        # grid numbers of notions chapter 10
python tests/test_pyramid.py     # cutting, cache key
RENDER2D=.../render2d.exe SHP=.../batiments.shp STYLE=.../batiments.yaml python tests/test_pyramid.py
```

The last one also checks the seams: two tiles on each side of a metatile
border, stitched, must match a direct render2d call over the same area.

## Layout

```
tiles2d/grid.py       z/x/y <-> envelope, metatiles (viewer2d will import it)
tiles2d/config.py     the map file
tiles2d/cache.py      the cache key
tiles2d/pyramid.py    render metatiles, cut, write
tiles2d/preview.py    index.html
tiles2d/main.py       command line and report
```

## Used by viewer2d

viewer2d's tile mode serves the same cache and fills it on demand. It imports
`grid.py`, `cache.layer_key` and `pyramid.render_metatile` from here, so a tile
pre-computed by tiles2d and a tile drawn by viewer2d are the same file. Use the
same `--out` folder as the `tiles:` line of `viewer.yaml`
(e.g. `C:/data/cache`).

`pip install -e .` makes tiles2d importable from anywhere (and adds a
`tiles2d` command); otherwise viewer2d finds it next to itself.

## Next

MBTiles output (one SQLite file, rows flipped: TMS convention).

## License

Copyright (C) 2026 Noël Frankinet.

tiles2d is free software: you can redistribute it and/or modify it under the
terms of the GNU Affero General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option) any
later version. See [`LICENSE`](../LICENSE).

It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.

For other licensing terms, contact the author.
