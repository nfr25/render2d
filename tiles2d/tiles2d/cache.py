# SPDX-License-Identifier: AGPL-3.0-or-later
"""The cache key: a fingerprint of everything that changes the drawing
(viewer2d notions, chapter 10).

Edit a style, replace a data file, rebuild render2d: the key changes, new
tiles go to a new folder, and old tiles can never be served by mistake.
"""
from __future__ import annotations

import hashlib
from pathlib import Path

from .config import DEFAULT_CRS, Layer

# A shapefile is several files: any of them changing changes the map.
_COMPANIONS = ('.shp', '.shx', '.dbf', '.prj', '.cpg', '.qix')
_KEY_VERSION = 1   # bump when the tile format itself changes


def _stat(h, path: Path) -> None:
    if path.is_file():
        st = path.stat()
        h.update(f'{path.name}|{st.st_size}|{st.st_mtime_ns}\n'.encode())


def layer_key(layer: Layer, render2d: str) -> str:
    """12 hex characters: short enough for a folder name, long enough to never collide."""
    h = hashlib.sha256(f'tiles2d-key-v{_KEY_VERSION}\n'.encode())
    _stat(h, Path(render2d))

    src = layer.source.split('::')[0]
    if src.startswith('PG:'):
        # A database: no file to watch. The connection string is part of the key;
        # after a data change, delete the folder (or use --force).
        h.update(src.encode())
    else:
        p = Path(src)
        for ext in _COMPANIONS:
            _stat(h, p.with_suffix(ext))
        if p.suffix.lower() not in _COMPANIONS:
            _stat(h, p)   # GeoPackage, GeoJSON...
    if '::' in layer.source:
        h.update(layer.source.split('::', 1)[1].encode())

    if layer.style:
        try:
            h.update(Path(layer.style).read_bytes())    # the content, not the date
        except OSError:
            h.update(b'missing-style')
    else:
        h.update(b'default-style')
    # Another system, other tiles: 17/x/y covers other ground in Mercator.
    # Only when not the default, so that the Lambert pyramids already cut keep their key.
    if layer.map_crs != DEFAULT_CRS or layer.crs:
        h.update(f'crs {layer.map_crs} {layer.crs or ""}'.encode())
    return h.hexdigest()[:12]
