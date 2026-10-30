# SPDX-License-Identifier: AGPL-3.0-or-later
"""Building a pyramid: render metatiles with render2d, cut them into tiles.

One metatile = one render2d call for n x n tiles. Features crossing tile
borders are drawn once, so neighbouring tiles always match.
"""
from __future__ import annotations

import os
import subprocess
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image

from . import grid
from .config import Layer

# render2d loads GDAL, which on MSYS2 brings OpenBLAS. By default OpenBLAS
# prepares one thread (and its memory buffers) per core in EVERY process:
# 32 render2d x 32 threads exhausts memory ("Memory allocation still failed").
# render2d does not need them: one thread per process, the parallelism is ours.
RENDER_ENV = {**os.environ, 'OPENBLAS_NUM_THREADS': '1', 'OMP_NUM_THREADS': '1'}


@dataclass
class ZoomStats:
    z: int
    metatiles: int = 0          # rendered
    skipped: int = 0            # already complete in the cache
    tiles: int = 0              # written
    empty: int = 0              # written, fully transparent
    errors: int = 0             # still failing after one retry
    retried: int = 0            # failed once, succeeded on retry
    seconds: float = 0.0
    render_seconds: float = 0.0  # sum of render2d times (> seconds when parallel)


@dataclass
class LayerStats:
    layer: str
    key: str
    zooms: list[ZoomStats] = field(default_factory=list)
    warnings: set[str] = field(default_factory=set)
    first_error: str = ''


def tile_path(root: Path, z: int, tx: int, ty: int) -> Path:
    return root / str(z) / str(tx) / f'{ty}.png'


def render_command(render2d: str, layer: Layer, bbox: grid.Envelope, size: int, out: str) -> list[str]:
    cmd = [render2d, layer.source]
    if layer.style:
        cmd += ['--style', layer.style]
    # The grid is in the metres of the map's system (Lambert 72 by default):
    # render2d is always told which, and converts a layer in another (an
    # OpenStreetMap extract in degrees). Without it, a layer in degrees alone
    # would make render2d choose Web Mercator (render2d §70). render2d 0.4.5.
    if layer.crs:
        cmd += ['--layer-crs', layer.crs]
    cmd += ['--crs', layer.map_crs]
    cmd += ['--bbox', *(f'{v:.6f}' for v in bbox), '--size', f'{size}x{size}',
            '--background', 'none', '-o', out]
    return cmd


def _save_atomic(img: Image.Image, path: Path) -> None:
    """Write next to the target, then rename: a server reading the cache at the
    same time never sees a half-written tile."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f'.{os.getpid()}.{threading.get_ident()}.tmp')
    img.save(tmp, format='PNG', optimize=True)
    os.replace(tmp, path)


def cut_metatile(img: Image.Image, root: Path, z: int, mx: int, my: int, n: int) -> tuple[int, int]:
    """Cuts an (n*256)^2 image into n*n tiles. Returns (written, empty)."""
    written = empty = 0
    s = grid.TILE_SIZE
    for tx, ty, i, j in grid.metatile_tiles(mx, my, n):
        tile = img.crop((i * s, j * s, (i + 1) * s, (j + 1) * s))
        if tile.getchannel('A').getbbox() is None:
            empty += 1          # still written: the server will not recompute it
        _save_atomic(tile, tile_path(root, z, tx, ty))
        written += 1
    return written, empty


def render_metatile(render2d: str, layer: Layer, root: Path, z: int, mx: int, my: int, n: int):
    """Renders and cuts one metatile: (written, empty, seconds, warnings, error).
    Also used by viewer2d to fill the cache on demand."""
    size = n * grid.TILE_SIZE
    with tempfile.TemporaryDirectory(prefix='tiles2d-') as tmp:
        out = str(Path(tmp) / 'meta.png')
        cmd = render_command(render2d, layer, grid.metatile_bbox(z, mx, my, n), size, out)
        t0 = time.perf_counter()
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace',
                              env=RENDER_ENV)
        dt = time.perf_counter() - t0
        warnings = {line[len('warning: '):] for line in proc.stdout.splitlines() if line.startswith('warning: ')}
        if proc.returncode != 0 or not Path(out).is_file():
            msg = (proc.stdout + proc.stderr).strip().splitlines()
            return 0, 0, dt, warnings, f'{z}/{mx}/{my} (metatile): ' + (msg[-1] if msg else f'exit {proc.returncode}')
        with Image.open(out) as img:
            img = img.convert('RGBA')
            if img.size != (size, size):
                return 0, 0, dt, warnings, f'{z}/{mx}/{my}: image is {img.size}, expected {size}x{size}'
            written, empty = cut_metatile(img, root, z, mx, my, n)
    return written, empty, dt, warnings, ''


def _complete(root: Path, z: int, mx: int, my: int, n: int) -> bool:
    return all(tile_path(root, z, tx, ty).is_file() for tx, ty, _, _ in grid.metatile_tiles(mx, my, n))


def build_layer(render2d: str, layer: Layer, key: str, root: Path, bbox: grid.Envelope,
                zooms: range, n: int, jobs: int, force: bool, progress, zoom_done) -> LayerStats:
    stats = LayerStats(layer.name, key)
    for z in zooms:
        zs = ZoomStats(z)
        todo = []
        for mx, my in grid.metatiles_covering(bbox, z, n):
            if not force and _complete(root, z, mx, my, n):
                zs.skipped += 1
            else:
                todo.append((mx, my))
        t0 = time.perf_counter()
        failed = []
        with ThreadPoolExecutor(max_workers=jobs) as pool:      # threads only wait for render2d
            futures = {pool.submit(render_metatile, render2d, layer, root, z, mx, my, n): (mx, my)
                       for mx, my in todo}
            for k, f in enumerate(as_completed(futures), 1):
                written, empty, dt, warns, err = f.result()
                zs.render_seconds += dt
                stats.warnings |= warns
                if err:
                    failed.append((futures[f], err))
                else:
                    zs.metatiles += 1
                    zs.tiles += written
                    zs.empty += empty
                progress(layer.name, z, k, len(todo))
        # A failure can be transient (memory pressure): retry once, one at a time.
        for (mx, my), first in failed:
            written, empty, dt, warns, err = render_metatile(render2d, layer, root, z, mx, my, n)
            zs.render_seconds += dt
            if err:
                zs.errors += 1
                stats.first_error = stats.first_error or first
            else:
                zs.retried += 1
                zs.metatiles += 1
                zs.tiles += written
                zs.empty += empty
        zs.seconds = time.perf_counter() - t0
        stats.zooms.append(zs)
        zoom_done(zs)
    return stats
