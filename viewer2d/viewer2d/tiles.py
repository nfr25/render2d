# SPDX-License-Identifier: AGPL-3.0-or-later
"""Tile mode: serve the pyramids of the shared cache, fill it on demand
(notions §10).

Everything about the grid, the cache key and metatile rendering comes from
tiles2d: one implementation, so tiles drawn by tiles2d in advance and tiles
drawn here on demand are the same files, in the same folders.
"""
from __future__ import annotations

import asyncio
import re
import sys
import time
from pathlib import Path

from fastapi.responses import FileResponse, JSONResponse, Response

from .config import Config

try:                                    # installed: pip install -e ../tiles2d
    import tiles2d                      # noqa: F401
except ImportError:                     # or simply next to viewer2d, in the same folder
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tiles2d'))
try:
    import tiles2d                      # noqa: F811
    from tiles2d import cache as t2cache
    from tiles2d import grid
    from tiles2d.config import Layer as T2Layer, layer_name
    from tiles2d.pyramid import render_command, render_metatile, tile_path
except ImportError as e:                # tiles2d missing, or older than 1.1
    found = getattr(sys.modules.get('tiles2d'), '__file__', None)
    sys.exit(f'viewer2d: tile mode needs tiles2d 1.1 or later ({e}).\n'
             f'  tiles2d found at: {found or "nowhere"}\n'
             f'  unzip tiles2d-v1.1.zip next to viewer2d, or remove "tiles:" from viewer.yaml')

Z_MIN, Z_MAX = -10, 10                  # sane limits for z in a URL
KEY_RE = re.compile(r'^[0-9a-f]{12}$')


class TileService:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.root = Path(cfg.tiles_dir)
        self.n = cfg.metatile
        taken: set[str] = set()
        # render2d §70: each layer's declared system, and the map's (the tiles' own).
        self.layers = [T2Layer(l.source, l.style, layer_name(l.source, taken), l.crs, cfg.crs) for l in cfg.layers]
        self.by_name = {l.name: l for l in self.layers}
        self.slots = asyncio.Semaphore(cfg.max_parallel)   # same role as for /render.png
        self.locks: dict[tuple, asyncio.Lock] = {}          # one per metatile being drawn
        self._keys: dict[str, str] = {}
        self._keys_time = 0.0
        self.stats = {'served': 0, 'rendered_metatiles': 0, 'render_seconds': 0.0,
                      'errors': 0, 'last_error': ''}

    # --- cache keys ------------------------------------------------------
    def keys(self) -> dict[str, str]:
        """Current key of each layer. Recomputed at most once a second: editing a
        style file changes its key, and the page switches to the new pyramid."""
        now = time.monotonic()
        if now - self._keys_time > 1.0:
            self._keys = {l.name: t2cache.layer_key(l, self.cfg.render2d) for l in self.layers}
            self._keys_time = now
        return self._keys

    def layer_root(self, name: str, key: str) -> Path:
        return self.root / name / key

    # --- one tile ----------------------------------------------------------
    async def tile(self, name: str, key: str, z: int, x: int, y: int):
        layer = self.by_name.get(name)
        if layer is None or not KEY_RE.match(key) or not Z_MIN <= z <= Z_MAX:
            return Response('unknown layer, key or zoom', status_code=404, media_type='text/plain')
        if key != self.keys()[name]:
            # The style or the data changed since the page asked: the page will
            # pick the new key within 2 s. Never draw into an outdated folder.
            return Response('outdated key', status_code=410, media_type='text/plain')

        root = self.layer_root(name, key)
        path = tile_path(root, z, x, y)
        if not path.is_file():
            mx, my = grid.metatile_of(x, y, self.n)
            lock_id = (name, key, z, mx, my)
            lock = self.locks.setdefault(lock_id, asyncio.Lock())
            # Leaflet asks for the 16 tiles of a metatile at once: the first
            # request draws it, the 15 others wait here and then find their file.
            async with lock:
                if not path.is_file():
                    async with self.slots:
                        written, empty, dt, warns, err = await asyncio.to_thread(
                            render_metatile, self.cfg.render2d, layer, root, z, mx, my, self.n)
                    self.stats['render_seconds'] += dt
                    if err:
                        self.stats['errors'] += 1
                        self.stats['last_error'] = err
                    else:
                        self.stats['rendered_metatiles'] += 1
            if not lock.locked():
                self.locks.pop(lock_id, None)
            if not path.is_file():
                return Response(self.stats['last_error'] or 'render failed', status_code=502,
                                media_type='text/plain')
        self.stats['served'] += 1
        # A key never changes content: the browser may keep the tile.
        return FileResponse(path, media_type='image/png', headers={'Cache-Control': 'max-age=86400'})

    # --- what is under the mouse -------------------------------------------
    def info(self, x: float, y: float, z: int) -> dict:
        tx, ty = grid.tile_of(x, y, z)
        mx, my = grid.metatile_of(tx, ty, self.n)
        bbox = grid.tile_bbox(z, tx, ty)
        mbbox = grid.metatile_bbox(z, mx, my, self.n)
        keys = self.keys()
        files = {l.name: tile_path(self.layer_root(l.name, keys[l.name]), z, tx, ty).is_file()
                 for l in self.layers}
        size = self.n * grid.TILE_SIZE
        # The exact command that draws this metatile, to replay it by hand.
        commands = []
        for l in self.layers:
            cmd = render_command(self.cfg.render2d, l, mbbox, size, 'metatile.png')
            commands.append(' '.join(f'"{a}"' if ' ' in a else a for a in cmd))
        return {'tile': f'{z}/{tx}/{ty}', 'bbox': bbox, 'metatile': f'{z}/{mx}/{my}',
                'metatile_bbox': mbbox, 'size': size, 'cached': files, 'commands': commands}


def register(app, cfg: Config) -> TileService | None:
    if not cfg.tiles_dir:
        return None
    svc = TileService(cfg)

    @app.get('/tiles/status')
    async def status():
        return JSONResponse({'layers': [{'name': l.name, 'key': svc.keys()[l.name]} for l in svc.layers],
                             'stats': svc.stats, 'root': str(svc.root)},
                            headers={'Cache-Control': 'no-store'})

    @app.get('/tiles/info')
    async def info(x: float, y: float, z: int):
        return JSONResponse(svc.info(x, y, z), headers={'Cache-Control': 'no-store'})

    @app.get('/tiles/{name}/{key}/{z}/{x}/{y}.png')
    async def tile(name: str, key: str, z: int, x: int, y: int):
        return await svc.tile(name, key, z, x, y)

    return svc
