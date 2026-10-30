# render2d

A small command-line map renderer in the spirit of the GDAL tools: vector
layers in, a map out (PNG, SVG or PDF). Data is read with GDAL/OGR, drawing is
done with Cairo behind an abstract canvas, so another engine can replace Cairo
later.

The design notes, with the maths worked out step by step, are in
[`docs/notions.en.html`](docs/notions.en.html). They are the reference:
the code follows them.

## Status

| Step | Content | |
|---|---|---|
| 1 | World → pixel matrix, canvas, Cairo surfaces (PNG/SVG/PDF) | done |
| 2 | Reading layers with GDAL, fixed style per layer, `render2d` command | done |
| 3a | Styles from YSLD files (SLD model): symbolizers, scale ranges, passes | done |
| 3b | Attributes and filters in styles (CQL, `else` rules) | done |
| 3c | Labels: `text` symbolizer, halo, arithmetic, rotation from an attribute; `--pixel-ratio` | done |
| 3d | Symbols: well-known marks, size and rotation from attributes, PNG images, hatching | done |
| 3e | The map as a library (`r2d_map`, `CairoImage`), for editor2d; build option `R2D_BUILD_PROGRAMS` | done |
| 3f | Querying the map: FIDs, the same rules for drawing and querying, exact hit tests, `Map::queryAt`, `--at` | done |
| 3g | Querying a zone: segment crossing, the three cases of `intersects`, a cap, `Map::queryArea`, `--box`, `--area` | done |
| 3h | `--config viewer.yaml`: one configuration reader for render2d and editor2d (moved from editor2d) | done |
| 3i | A map that changes: `addLayer`, `removeLayer`, `moveLayer`; the default colour follows a layer that moves (for editor2d step 3a) | done |
| 3j | Writing YSLD: `writeYsld`, the round trip on every style of `styles/`, `Map::setLayerStyle` (for editor2d's legend dialog) | done |
| 3k | The style catalogue: what each symbolizer can receive, add and remove (editor2d's "+") | done |
| 3l | Writing the map file: `writeConfig`, paths relative to the file, viewer2d's keys kept (editor2d's project) | done |
| 3m | Classifying: the values of a field, equal intervals and quantiles, colour ramps, `classify()` | done |
| 3n | Classifying on the command line: `--categorize`, `--graduate`, `--write-style` | done |
| 3o | Selecting by attributes: `Map::queryWhere`, `--where` (for editor2d step 4a) | done |
| — | Label collisions, labels across tile borders | nice to have |
| — | Test viewer `viewer2d` (NiceGUI + Leaflet), a separate project calling `render2d` | done |
| 4a | Raster layers (GeoTIFF, world files): the view's window with overviews, RGB / RGBA / palette / grey, nodata, the `raster` symbolizer and its opacity | done |
| 4b-1 | Reprojection on the fly of vector layers (PROJ): `--crs`, `crs:` in the config, each layer converted to the map's system | done |
| 4b-2 | Reprojection of rasters (warping), OpenStreetMap tiles. Meanwhile: `gdalwarp -t_srs EPSG:31370 -r bilinear -of COG in.tif out.tif` once | postponed |
| 4c | WMS / WMTS / WFS sources | |
| — | Clipping, simplification | |
| 5 | Full command line, JSON warnings | |
| 6 | Raster tiles `z/x/y`, through a separate `tiles2d` tool that calls render2d per tile | |
| 7 | Vector tiles (MVT) | |

## Building

Windows with MSYS2, in the **UCRT64** terminal:

```
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake \
                   mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-cairo \
                   mingw-w64-ucrt-x86_64-gdal mingw-w64-ucrt-x86_64-yaml-cpp make
```

Then, at the project root:

```
make            # configure (CMake) and build, Debug, in build/
make test       # build and run the tests
make run        # step 1 demo scene: test.png, test.svg, test.pdf
make release    # optimized build in build-release/ (use it to measure speed)
make clean      # only when a build is inexplicable: everything is rebuilt
make fresh      # after unpacking a zip: dates the sources now, then test + release
```

When another project includes render2d with `add_subdirectory` (editor2d), only
the libraries are built: the programs and tests are skipped. The option
`R2D_BUILD_PROGRAMS` (default: ON when render2d is the main project) decides.

## Windows binary, without MSYS2

Each release on GitHub has a ready-made zip:
[Releases](https://github.com/nfr25/render2d/releases), `render2d-<version>-windows-x64.zip`.
Unzip it anywhere and run `render2d --version` in a command prompt: the DLLs
are next to the executable, and GDAL's and PROJ's reference files in `share\`
(found beside the executable since 0.4.4; `GDAL_DATA` and `PROJ_DATA`, when
set, still win). `--version` shows which folders were used.

To build that zip yourself, in the UCRT64 terminal:

```
tools/package-windows.sh     # build, copy DLLs and data, check without MSYS2, zip into dist/
```

Details in the notes, chapter 68.

## Characterization test

`tools/regress.sh` checks that a change meant to keep the output (like 0.3.5,
the map as a library) really keeps it (notions §49). In the MSYS2 terminal:

```
tools/regress.sh before     # with the executable you have now: regress/before/
make release                # the new version
tools/regress.sh after      # same commands, then compares: PNG byte for byte,
                            # reports without the version and the milliseconds
```

The commands are in `tools/regress-commands.txt` (adapt the data paths).

On Linux, the same commands work with the distribution packages for a C++20
compiler, CMake, pkg-config, Cairo, GDAL and yaml-cpp.

## Usage

Four ways to use it, all from the same `SOURCE`s, `--style`s and `--config`:

| To... | Options | Prints | Writes |
|---|---|---|---|
| draw a map | `--bbox`, `--size`, `--pixel-ratio`, `-o` | the report (layers, rules, times) | the image (PNG, SVG, PDF) |
| ask what is there | `--at X Y`, `--box`, `--area WKT`, `--where EXPRESSION` | the features found, with FID and attributes | an image only with `-o` |
| make a classified style | `--categorize FIELD`, `--graduate FIELD`, `--classes`, `--method`, `--colours` | the legend: colour, count, rule | the style with `--write-style`; an image with `-o` |
| use a viewer2d / editor2d map | `--config viewer.yaml` | the layers read, paths resolved | (as above) |

The styles are YSLD files (GeoServer's YAML form of SLD): written by hand,
by editor2d's legend dialog, or by `--categorize` / `--graduate`; every tool
of the family reads them the same way.

```
render2d SOURCE [SOURCE ...] [options]
```

| Argument | Meaning |
|---|---|
| `--config FILE.yaml` | The layers (`source`, `style`, `visible`) and `start: bbox` of a viewer.yaml, such as viewer2d's. Relative paths are taken from the file's folder. viewer2d's own keys are ignored; any other key gives a warning (a typo, often). `SOURCE`s given as well are drawn on top; `--bbox` replaces `start: bbox`. The report starts with the file and every path it resolved. |
| `SOURCE` | A file or a GDAL connection string: a vector layer, or a raster (GeoTIFF...; step 4a). `SOURCE::layer` picks a layer, otherwise the first one is used. Layers are drawn in the order given, the first at the bottom. |
| `--style FILE.yaml` | YSLD style for the `SOURCE` just before it. Without it, a built-in style chosen from the geometry type. |
| `--crs CRS` | The map's coordinate system, in metres: `EPSG:31370`, `EPSG:3857`, a UTM zone... Default: `crs:` of `--config`, else the first layer's (`EPSG:3857` if that one is in degrees). A layer in another system is converted as it is read; the report says how (notes §69 to §71). |
| `--layer-crs CRS` | The system of the SOURCE just before (as `--style`), when its file says none or a wrong one: `EPSG:31300`, `EPSG:4326`... The file is not changed. In a config file: `crs:` on the layer. |
| `--bbox XMIN YMIN XMAX YMAX` | Area to draw, in the map's coordinate system. Default: total extent of the layers. |
| `--size W` or `--size WxH` | Image size. Default: 1000 wide, height following the shape of the area. If both are given and the shapes differ, the area is widened so that pixels stay square. |
| `--background RRGGBB` or `none` | Background colour, default `FFFFFF`. `none` gives a transparent image. |
| `--version` | Prints the version of render2d (`0.<step>.<sub-step>`: 0.3.4 is step 3d), its build date, and the versions of GDAL and Cairo. The report also starts with the version: an old executable is spotted at once. |
| `--pixel-ratio R` | The image will be seen R times smaller (1.5 on a 4K screen scaled to 150 %): every length of the style in pixels is multiplied by R, and the scale is computed for the pixels seen. Default 1. |
| `-o FILE` | Output file. The extension chooses the format: `.png`, `.svg` or `.pdf`. Default: `map.png`. |
| `--at X Y` | What is at this point of the map (map units): the features the style draws there, top layer first, with their FID and attributes. `--bbox` and `--size` set the scale. No image unless `-o` is given. |
| `--box XMIN YMIN XMAX YMAX` | What is in this rectangle: the features the style draws whose geometry touches it (OGC `intersects`), top layer first. Same rules as `--at` for the scale and `-o`. |
| `--area WKT` | The same in a polygon: `--area "POLYGON((x y, x y, x y, x y))"`. |
| `--where EXPRESSION` | The features of the last SOURCE that make the expression true, in the language of the styles' filters: `--where "AREA > 500 AND TYPE = 'maison'"`. Every feature, whatever the scale. Printed like `--at`; no image unless `-o`. |
| `--categorize FIELD` | A style for the last SOURCE: one colour per value of FIELD, then `(other)` in grey. The SOURCE's `--style` gives the symbol (hatch, outline...); only the colour changes. Prints the legend; no image unless `-o`. Refused beyond 50 values. |
| `--graduate FIELD` | The same in classes of a number, from light to dark. `--classes N` (2 to 10, default 5), `--method equal` or `quantiles`, `--colours RRGGBB RRGGBB` (default `FFF5EB 8C2D04`). |
| `--write-style FILE.yaml` | Writes the style made by `--categorize` / `--graduate`: an ordinary YSLD file, for `--style`, viewer.yaml, viewer2d or editor2d. |

### Examples

```
# Whole extent, parcels below, buildings on top
./build/render2d /c/data/parcelles.shp /c/data/batiments.shp -o map.png

# A zoom: area in Lambert 72 metres, explicit size
./build/render2d /c/data/parcelles.shp /c/data/batiments.shp \
    --bbox 150000 160000 152000 161500 --size 1200x900 -o zoom.png

# The same map as a PDF: only the extension changes
./build/render2d /c/data/parcelles.shp -o map.pdf

# With styles: parcel boundaries and building outlines only when zoomed in
./build/render2d /c/data/parcelles.shp --style styles/parcelles.yaml \
                 /c/data/batiments.shp --style styles/batiments.yaml -o map.png

# One layer of a GeoPackage, transparent background
./build/render2d commune.gpkg::batiments --background none -o buildings.png
```

### The map of a file: --config

```
# The map of viewer2d, start view, 1200 wide
render2d --config ../viewer2d/viewer.yaml --size 1200 -o bruxelles.png

# The same layers, another place; plus the trees on top
render2d --config ../viewer2d/viewer.yaml \
    --bbox 152200 169200 152600 169500 --size 1200x900 \
    C:/data/topo/shp/arbres.fgb -o rue.png
```

The report then begins with what was read:

```
render2d 0.3.8
config: D:/work/render2d/viewer2d/viewer.yaml
  layer 1: C:/data/shp/UrbISBuildings_04000_CadastralParcels.fgb
           style D:/work/render2d/render2d/styles/parcelles.yaml
  ...
```

### Questions: what is here, what is in this zone

`--at`, `--box` and `--area` ask instead of drawing. `--bbox` and `--size`
still matter: they set the scale, hence which rules are active and which
features are visible (only what is drawn answers). Without `-o`, only the
answer is printed, no image; with `-o`, the image is written too. The zone
itself is never drawn on the image.

Tried on UrbIS (0.3.7): a rectangle of 80 m around houses selected in editor2d.

```
render2d C:/data/shp/UrbISBuildings_04000_Buildings.fgb \
    --bbox 152200 169200 152600 169500 --size 1200x900 --box 152350 169320 152430 169400
```
```
render2d 0.3.7
in box 152350.00 169320.00 152430.00 169400.00 (scale 1:1190): 39 hits
  UrbISBuildings_04000_Buildings  fid 40705  INSPIRE_ID="https://databrussels.be/id/building/1815413"  BLOCK_ID="https://databrussels.be/id/block/25733"  AREA=106
  ...
  UrbISBuildings_04000_Buildings  fid 40698  INSPIRE_ID="https://databrussels.be/id/building/1827640"  BLOCK_ID="https://databrussels.be/id/block/25733"  AREA=98
  ...
```

Among the 39: fids 40616, 40697 and 40698, the very ones clicked in editor2d
(same code, same FIDs). Other forms:

```
# What is at this point (tolerance 4 px)?
render2d .../UrbISBuildings_04000_Buildings.fgb --bbox 152200 169200 152600 169500 --size 1200x900 \
    --at 152391.36 169358.63

# In a polygon (WKT, as PostGIS); -o also draws the map
render2d .../UrbISBuildings_04000_Buildings.fgb --bbox 152200 169200 152600 169500 --size 1200x900 \
    --area "POLYGON((152350 169320, 152430 169320, 152390 169400, 152350 169320))" -o zone.png
```

A rectangle on a park or a railway answers `0 hits`: nothing is wrong, there
is simply no building there.

In MSYS2, `C:\data\parcelles.shp` is written `/c/data/parcelles.shp`
(`C:/data/parcelles.shp` also works).

For faster zooms on shapefiles, create a spatial index once:

```
ogrinfo parcelles.shp -sql "CREATE SPATIAL INDEX ON parcelles"
```

### Classified maps: --categorize, --graduate (step 3n)

The assistants of editor2d's legend dialog, without a window: the same code
(`style/classify.hpp`), the same result. The style made is an ordinary YSLD
file: viewer2d and editor2d draw it like any other.

```
# The legend only: 5 classes of surface, as many buildings in each
render2d .../UrbISBuildings_04000_Buildings.fgb --graduate AREA --method quantiles

# Keep the hatch and outline of a style, write the classified one, and draw it
render2d .../UrbISBuildings_04000_Buildings.fgb --style styles/batiments.yaml \
    --graduate AREA --classes 4 --write-style styles/batiments-surface.yaml -o surface.png

# One colour per type of topographic point
render2d .../topo/shp/arbres.fgb --categorize TYPE
```

The legend printed (here on a test file of 400 buildings):

```
graduate AREA: 400 features, 0 without value (read in 3 ms)
  #FFF5EB       100  < 276.6
  #D9B29E       100  276.6 - 445.5
  #B27051       100  445.5 - 722.2
  #8C2D04       100  >= 722.2
  #CCCCCC         0  (other)
```

### PostGIS

A PostGIS table is a `SOURCE` like a file, through GDAL's PostgreSQL driver:

```bash
render2d "PG:host=db.example.org dbname=gis user=gisuser connect_timeout=5::municipalities" -o communes.png
```

`::` picks the table (`::schema.table` outside `public`). Keep the password out
of the command and of the map file: put it in libpq's `pgpass.conf`
(`%APPDATA%\postgresql\pgpass.conf` on Windows). Chapter 67 of
`docs/notions.en.html` is a cheat sheet: Oracle Spatial and its PostGIS
equivalents, `ogr2ogr` recipes, checks in `psql`.

### Raster layers (step 4a)

A GeoTIFF (or any raster GDAL reads) is a layer like the others, drawn at
its rank: usually first, so that the vector layers are drawn over it.

```bash
# An orthophoto under the buildings, faded to 60 %
render2d ortho.tif --style styles/ortho.yaml batiments.shp --bbox 148000 170000 148400 170300

# A terrain or surface model: grey from its lowest to its highest value
render2d UrbDSM_2021_147171.tif -o dsm.png
```

Only the pixels of the view are read, at the size of the image: averaged when
zoomed out (from the file's overviews when it has some), the nearest file pixel
when zoomed in. Three bands are red, green, blue (a fourth may be an alpha);
one band with a palette uses it; one band of numbers is drawn in grey, from
the smallest to the largest value of the whole file. Nodata is transparent.
The position comes from the file or from its world file (`.tfw`).

```yaml
name: ortho
feature-styles:
- rules:
  - scale: [min, 20000]
    symbolizers:
    - raster:
        opacity: 0.6
```

The report gives the pixels drawn and the file pixels they cover:

```
  UrbDSM_2021_147171   raster 1000x1000 px from 2000x2000 file px (averaged)     148.2 ms  (read 147.1 ms, draw 1.1 ms)
```

A raster without a coordinate system is assumed to be in the map's (with a
warning); a raster in another system is not drawn until step 4b. A large raster
(over 4096 pixels on a side) without overviews gets a warning with the command
that adds them: `gdaladdo -r average file.tif 2 4 8 16 32 64`.

### Styles

Styles are written in YSLD, the YAML form of OGC SLD defined by GeoServer.
Step 3a supports `polygon`, `line` and `point` (circle marks) symbolizers,
scale ranges (`scale: [min, 15000]`) and several feature-styles (one rendering
pass each). Lengths are in pixels. See [`styles/`](styles/) for examples and
chapters 22 to 44 of `docs/notions.en.html` for the details.

```yaml
name: batiments
feature-styles:
- rules:
  - name: proche
    scale: [min, 15000]
    symbolizers:
    - polygon: {fill-color: '#C98B6B', stroke-color: '#7A4A35', stroke-width: 0.8}
  - name: loin
    scale: [15000, max]
    symbolizers:
    - polygon: {fill-color: '#8E7B6B'}
```

#### Filters (step 3b)

A rule can have a `filter`, a CQL condition on the feature's attributes, and a
rule with `else: true` takes the features no other rule of the same pass drew.
Supported: `=`, `<>`, `<`, `<=`, `>`, `>=`, `BETWEEN … AND …`, `IN (…)`,
`LIKE` (`%`, `_`), `IS [NOT] NULL`, `AND`, `OR`, `NOT`, parentheses. Texts go
between apostrophes (`'21006'`, `'Clos de l''Oasis'`).

```yaml
- rules:
  - name: portes
    filter: ${PARENTID IS NULL}          # one point per door, not per flat
    symbolizers: [{point: {size: 5}}]
  - name: evere
    filter: "${MUNNISCODE = '21006'}"    # quotes: safe inside YAML
    symbolizers: [{polygon: {fill-color: '#E6A15C'}}]
  - name: autres
    else: true
    symbolizers: [{polygon: {fill-color: '#E4E1DA'}}]
```

- **The literal decides the comparison**: `POLICENUM < 80` compares numbers
  (`'75B'` is then unknown, and counted in a warning); `POLICENUM < '80'`
  compares texts, letter by letter.
- **Three-valued logic, as in SQL**: a comparison with NULL is unknown, and a
  rule applies only when its filter is true. Use `IS NULL` or an `else` rule to
  catch NULL values.
- **Only the fields the active filters use are read** from the data source
  (GDAL `SetIgnoredFields`); a style without filters reads no attribute at all.
- An unreadable filter is reported with its line and column, and its rule is
  never applied; a field the layer does not have counts as NULL, with a warning
  listing the available fields.

#### Symbols (step 3d)

```yaml
- point:
    size: ${4 * sqrt(N)}             # pixels; or a number. Functions: sqrt, abs
    rotation: ${90 - ANGLE}          # degrees clockwise; a mark at 0 points north
    uom: metre                       # optional: size in ground metres, not pixels
    symbols:                         # the first one that can be drawn
    - external: {url: icons/arbre.png, format: image/png}   # relative to the style file
    - mark: {shape: triangle, fill-color: '#2F7D4F', stroke-color: '#FFFFFF', stroke-width: 1}
- polygon:
    fill-color: '#F2EBDD'
    fill-graphic:                    # hatching: a tile repeated, anchored to the world origin
      size: 8
      symbols:
      - mark: {shape: 'shape://slash', stroke-color: '#C98B6B', stroke-width: 1}
```

Shapes: `circle`, `square`, `triangle`, `star`, `cross`, `x`, and for hatching
`shape://slash`, `backslash`, `horline`, `vertline`, `times`, `dot`. Only PNG
images for now (SVG later). A missing image is reported and the next symbol,
usually a mark, is drawn instead.

#### Labels (step 3c)

A `text` symbolizer writes a value taken from the attributes. Labels are
collected while the layers are drawn and drawn at the end, above every layer.

```yaml
- name: numeros
  filter: ${PARENTID IS NULL}           # one number per door
  scale: [min, 1100]                    # below, two numbers 6.1 m apart overlap
  symbolizers:
  - text:
      label: ${POLICENUM}               # an expression, or a fixed text: label: Evere
      font-family: Arial                # resolved by the system, falls back silently
      font-size: 9                      # pixels
      font-weight: normal               # or bold; font-style: italic
      fill-color: '#3A2A20'
      halo: {radius: 1.5, fill-color: '#FFFFFF'}
      anchor: [0.5, 0.5]                # SLD default [0, 0.5]: the text starts at the point
      displacement: [0, 0]              # pixels, y up
      rotation: ${360 - CARTOANGLE}     # degrees clockwise (SLD)
```

- Expressions accept arithmetic (`+ - * /`, unary minus, usual priorities);
  a division by zero or a text that is not a number gives NULL.
- A label that would be upside down (between 90° and 270°) is turned by 180°.
- Glyphs are drawn as outlines (`cairo_text_path`): crisp in SVG and PDF, but
  not selectable text.
- Placement: on the point; at the middle of a line; at the centre of a
  polygon's envelope. No collision handling yet: overlapping labels are all drawn.

Colours must be quoted (`'#C98B6B'`): in YAML, `#` starts a comment. Unknown
keys and unreadable values are reported with their line and skipped; a file
that is not valid YAML falls back to the built-in style, with a warning.

### What it prints

```
map: EPSG:31370 (BD72 / Belgian Lambert 72), 1000x800 px, 0.500 m/px, scale 1:1786
  parcelles                 252 read      252 drawn       9.3 ms  (read 1.2 ms, draw 8.1 ms, 1 pass)
  batiments                 136 read      136 drawn       3.3 ms  (read 0.4 ms, draw 2.9 ms, 1 pass)
wrote zoom.png (31.0 ms)
warning: cannot open 'absent.shp'
```

With filters, each layer also lists the fields read and what each rule drew:

```
  UrbISBuildings_21006_Addresses    22293 read    22293 drawn      77.8 ms  (read 22.0 ms, filters 1.7 ms, draw 54.2 ms, 1 pass)
      fields read: PARENTID
      rule portes                       6282 drawn
```

With labels, the report also gives the labels collected per layer, and their drawing time at the end:

```
      labels       50 (0 without text)
  labels 50 drawn 42.8 ms
```

Per layer: features read and drawn, total time, split into "read" (GDAL and
conversion), "filters" (only with filters) and "draw" (symbolizers and Cairo), and the number of passes read
(a pass with no rule active at this scale is skipped). Then the time to write the
file (PNG compression, or SVG/PDF completion).

Problems do not stop the map: a missing layer, a missing or different
coordinate system, empty or unsupported geometries are reported as warnings,
and everything else is drawn. Exit code: 0 when the file was written, 1 when
nothing could be drawn or written, 2 for a command-line error.

### Current limits (step 4a)

- No reprojection yet: the map uses the first coordinate system declared;
  vector layers in another system are drawn as they are, with a warning;
  rasters in another system are not drawn (step 4b).
- Rasters: no colour map for terrain models yet (grey only), no rotated
  rasters. No WMS/WFS yet (step 4c).
- Labels: no collision handling and no placement along lines yet.
- Images: PNG only (no SVG yet); hatching with marks only (no image tiles).
- Functions in expressions: `sqrt` and `abs` only, besides arithmetic.
- Geometry collections are skipped (and counted).

## Layout

```
src/core/           pure maths, no dependency: Point, Envelope, Affine, View, Geometry, Attributes
src/canvas/         the drawing interface, no Cairo: Canvas, styles, circle
src/backend/cairo/  the only code that includes Cairo
src/backend/gdal/   the only code that includes GDAL (vector layers, rasters)
src/style/          the SLD model, CQL expressions, YSLD reader and writer, the catalogue, classifying
src/render/         symbolizers, rendering passes and labels: geometry + style -> canvas
src/map/            the map as a library (layers kept open, the report, queries), viewer.yaml read and written
styles/             example YSLD styles
src/app/            render2d (the command), render2d_demo (step 1 scene)
tests/              one test program per library, run by `make test`
tools/              regress.sh (characterization test), package-windows.sh (the Windows zip)
docs/notions.en.html   design notes
```

Dependencies go one way: `core` knows nothing, `canvas` knows `core`,
`style` knows `canvas` and `core`, `render` knows `canvas` and `style`, and only
the backends know Cairo, GDAL or yaml-cpp.

## License

Copyright (C) 2026 Noël Frankinet.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU Affero General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option) any
later version. See [`LICENSE`](../LICENSE).

It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.

For other licensing terms (for example, use in a closed product or service),
contact the author.
