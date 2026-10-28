# SPDX-License-Identifier: AGPL-3.0-or-later
"""tiles2d: builds tile pyramids by calling render2d (viewer2d notions, chapter 10).

    python -m tiles2d --config viewer.yaml --zmin -4 --zmax 0 --out cache
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from . import cache, config, grid, preview
from .pyramid import build_layer


def _parse_bbox(values):
    try:
        b = tuple(float(v) for v in values)
    except ValueError:
        sys.exit('tiles2d: --bbox expects four numbers')
    if not (b[0] < b[2] and b[1] < b[3]):
        sys.exit('tiles2d: --bbox: xmin < xmax and ymin < ymax expected')
    return b


def main() -> None:
    p = argparse.ArgumentParser(prog='tiles2d', description='Builds tile pyramids with render2d.')
    p.add_argument('--config', default='viewer.yaml', help='map file, same format as viewer.yaml')
    p.add_argument('--zmin', type=int, required=True, help='first zoom level (e.g. -4)')
    p.add_argument('--zmax', type=int, required=True, help='last zoom level (e.g. 0)')
    p.add_argument('--bbox', nargs=4, metavar=('XMIN', 'YMIN', 'XMAX', 'YMAX'),
                   help='area to tile (default: start.bbox of the map file)')
    p.add_argument('--out', default='cache', help='output folder (default: cache)')
    p.add_argument('--layer', action='append', help='only this layer (repeatable); default: all')
    p.add_argument('--metatile', type=int, default=4, help='tiles per metatile side (default 4)')
    p.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 8),
                   help='render2d at the same time (default: cores, at most 8)')
    p.add_argument('--force', action='store_true', help='redraw tiles already in the cache')
    a = p.parse_args()

    try:
        cfg = config.load(a.config)
    except config.ConfigError as e:
        sys.exit(f'tiles2d: {e}')
    if a.zmin > a.zmax:
        sys.exit('tiles2d: --zmin must not be larger than --zmax')
    if a.metatile < 1 or a.metatile > 16:
        sys.exit('tiles2d: --metatile must be between 1 and 16')
    bbox = _parse_bbox(a.bbox) if a.bbox else cfg.bbox
    if bbox is None:
        sys.exit('tiles2d: no area: give --bbox or start.bbox in the map file')

    layers = cfg.layers
    if a.layer:
        layers = [l for l in cfg.layers if l.name in a.layer]
        missing = set(a.layer) - {l.name for l in layers}
        if missing:
            sys.exit(f"tiles2d: unknown layer(s) {', '.join(sorted(missing))}; "
                     f"available: {', '.join(l.name for l in cfg.layers)}")

    out = Path(a.out)
    zooms = range(a.zmin, a.zmax + 1)
    n = a.metatile

    # What is about to happen, before it happens.
    total_tiles = 0
    for z in zooms:
        cols, rows = grid.tiles_covering(bbox, z)
        total_tiles += len(cols) * len(rows)
    print(f'area {bbox[0]:.0f} {bbox[1]:.0f} {bbox[2]:.0f} {bbox[3]:.0f}, zoom {a.zmin}..{a.zmax}, '
          f'{total_tiles} tiles per layer in the area, metatiles {n}x{n}, {a.jobs} jobs')

    interactive = sys.stdout.isatty()

    def progress(name, z, k, total):
        # A live counter only in a real terminal; a log file gets the summaries only.
        if interactive and total and (k == total or k % max(1, total // 20) == 0):
            print(f'\r  zoom {z:>3}: {k}/{total} metatiles', end='', flush=True)

    def zoom_done(zs):
        if interactive:
            print('\r' + ' ' * 40 + '\r', end='')
        print(f'  zoom {zs.z:>3}: {zs.tiles:6d} tiles ({zs.empty} empty) from {zs.metatiles} metatiles, '
              f'{zs.skipped} metatiles already cached, {zs.errors} errors'
              + (f' ({zs.retried} recovered on retry)' if zs.retried else '')
              + f', {zs.seconds:6.1f} s', flush=True)

    for layer in layers:
        src = Path(layer.source.split('::')[0])
        if src.suffix.lower() == '.shp' and src.is_file() and not src.with_suffix('.qix').is_file():
            print(f'hint: {src.name} has no spatial index: every metatile reads the whole file. Create one with\n'
                  f'      ogrinfo "{src}" -sql "CREATE SPATIAL INDEX ON {src.stem}"')

    t_start = time.perf_counter()
    shown = []
    errors = 0
    for layer in layers:
        key = cache.layer_key(layer, cfg.render2d)
        root = out / layer.name / key
        print(f'{layer.name} -> {root}')
        st = build_layer(cfg.render2d, layer, key, root, bbox, zooms, n, a.jobs, a.force, progress, zoom_done)
        errors += sum(zs.errors for zs in st.zooms)
        for w in sorted(st.warnings):
            print(f'  warning (render2d): {w}')
        if st.first_error:
            print(f'  first error: {st.first_error}')

        meta = {'layer': layer.name, 'source': layer.source, 'style': layer.style, 'key': key,
                'tile_size': grid.TILE_SIZE, 'crs': 'Leaflet L.CRS.Simple, 1 map unit = 1 px at zoom 0',
                'bbox': list(bbox), 'zooms': [a.zmin, a.zmax], 'metatile': n,
                'created': datetime.now(timezone.utc).isoformat(timespec='seconds')}
        (root / 'tiles.json').write_text(json.dumps(meta, indent=2), encoding='utf-8')
        shown.append((layer.name, f'{layer.name}/{key}'))

    page = preview.write(out, shown, bbox, a.zmin, a.zmax)
    print(f'done in {time.perf_counter() - t_start:.1f} s; preview: {page}')
    if errors:
        print('some metatiles failed: run the same command again, only the missing ones are drawn')
    sys.exit(1 if errors else 0)
