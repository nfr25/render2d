# SPDX-License-Identifier: AGPL-3.0-or-later
"""Reading and checking viewer.yaml (notions §8, §11).

The browser never sends file names: every path comes from this file,
on the server side. Since version 1.2, render2d reads the layers itself
(--config); this reader keeps what viewer2d needs for its page and for the
tile mode, with the SAME rule as render2d for relative paths (§11).
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

import yaml


class ConfigError(Exception):
    """The configuration cannot be used at all."""


@dataclass
class Layer:
    source: str                 # a file or a GDAL connection string ("PG:..."), resolved (§11)
    style: str | None = None    # YSLD file, or None for the built-in style
    visible: bool = True        # visible: false -> not drawn at start (§11)
    title: str | None = None    # a readable name for the page
    crs: str | None = None      # its system, declared (render2d §70); None = the file's

    def label(self) -> str:
        """The title, or the file name."""
        return self.title or self.source.replace('\\', '/').split('/')[-1]


@dataclass
class Config:
    path: str                   # the file read, absolute: render2d --config reads it too
    render2d: str
    layers: list[Layer]
    start_bbox: tuple[float, float, float, float]
    max_parallel: int = 2                        # render2d processes at the same time
    warnings: list[str] = field(default_factory=list)
    tiles_dir: str | None = None                 # tile cache shared with tiles2d; None = no tile mode
    metatile: int = 4                            # same default as tiles2d
    crs: str = 'EPSG:31370'                      # the map's system, in metres (render2d §70)


def _bbox(value, where: str) -> tuple[float, float, float, float]:
    try:
        xmin, ymin, xmax, ymax = (float(v) for v in value)
    except (TypeError, ValueError):
        raise ConfigError(f'{where}: expected [xmin, ymin, xmax, ymax]') from None
    if not (xmin < xmax and ymin < ymax):
        raise ConfigError(f'{where}: xmin < xmax and ymin < ymax expected')
    return xmin, ymin, xmax, ymax


# viewer.yaml's keys: render2d warns about any other one, so do we.
KNOWN_KEYS = {'render2d', 'crs', 'layers', 'start', 'max_parallel', 'tiles', 'metatile'}

# The map's system when viewer.yaml names none. The page's Leaflet map is a
# plain plane (L.CRS.Simple, §4): any system in metres fits it, Lambert 72
# by default. render2d is always told which (--crs) and converts the layers
# in other systems (render2d §70): an OSM extract alone, in degrees, would
# otherwise make it choose Web Mercator on its own.
DEFAULT_CRS = 'EPSG:31370'
LAYER_KEYS = {'source', 'style', 'visible', 'title', 'crs'}


def resolve(value: str, folder: Path) -> str:
    """A path written in the file, read from the file's folder (§11, render2d §57).
    Kept as written: connection strings (PG:...), full paths (C:/..., /data/...).
    In "file.gpkg::layer", only the part before "::" is a path."""
    if value.startswith('PG:'):
        return value
    file, sep, layer = value.rpartition('::')
    if not sep:
        file, layer = value, ''
    if Path(file).anchor:                    # "C:/...", "C:", "/data/...": kept
        return value
    full = Path(os.path.normpath(folder / file)).as_posix()   # "a/../b" -> "b"
    return full + (sep + layer if sep else '')


def load(path: str | Path) -> Config:
    """Reads viewer.yaml. Missing files are warnings (the map still opens);
    a missing render2d, no layer or a bad bbox are errors."""
    path = Path(path)
    try:
        data = yaml.safe_load(path.read_text(encoding='utf-8'))
    except OSError as e:
        raise ConfigError(f'cannot read {path}: {e}') from None
    except yaml.YAMLError as e:
        raise ConfigError(f'{path} is not valid YAML: {e}') from None
    if not isinstance(data, dict):
        raise ConfigError(f'{path}: expected a map (render2d, layers, start)')

    warnings: list[str] = []
    # Absolute, but NOT resolve(): on Windows, resolve() turns a mapped drive
    # (Z: on a NAS) into its network path (//synology/...), which render2d,
    # given --config with it, read as Z:/synology/... (30 October).
    full = Path(os.path.abspath(path))
    folder = full.parent
    for key in data:
        if key not in KNOWN_KEYS:
            warnings.append(f"'{key}' is unknown, ignored")

    # crs: the map's system (render2d §70), checked by render2d itself.
    crs = str(data.get('crs') or DEFAULT_CRS).strip()

    exe = data.get('render2d')
    if not exe:
        raise ConfigError("'render2d': path to the render2d executable is missing")
    if not Path(exe).is_file():
        raise ConfigError(f"'render2d': file not found: {exe}")

    layers: list[Layer] = []
    for i, item in enumerate(data.get('layers') or []):
        if not isinstance(item, dict) or 'source' not in item:
            raise ConfigError(f"layers[{i}]: expected 'source' (and optionally 'style')")
        for key in item:
            if key not in LAYER_KEYS:
                warnings.append(f"layers[{i}]: '{key}' is unknown, ignored")
        visible = item.get('visible', True)
        if not isinstance(visible, bool):
            raise ConfigError(f"layers[{i}]: 'visible' must be true or false")
        layer = Layer(resolve(str(item['source']), folder),
                      resolve(str(item['style']), folder) if item.get('style') else None,
                      visible, str(item['title']) if item.get('title') else None,
                      str(item['crs']) if item.get('crs') else None)
        # Connection strings (PG:...) are not files: only check real paths.
        if not layer.source.startswith('PG:') and not Path(layer.source.split('::')[0]).exists():
            warnings.append(f'layer source not found: {layer.source}')
        if layer.style and not Path(layer.style).is_file():
            warnings.append(f'style not found: {layer.style} (render2d will use its default)')
        layers.append(layer)
    if not layers:
        raise ConfigError("'layers': at least one layer is needed")

    start = data.get('start') or {}
    if 'bbox' not in start:
        raise ConfigError("'start: bbox:' is needed (the first view, in the map's system: crs:)")
    bbox = _bbox(start['bbox'], 'start.bbox')

    max_parallel = int(data.get('max_parallel', 2))

    # Optional tile mode (notions §10): the cache folder tiles2d writes into.
    tiles_dir = data.get('tiles')
    if tiles_dir:
        tiles_dir = str(tiles_dir)
        if not Path(tiles_dir).is_dir():
            warnings.append(f'tiles folder not found, it will be created: {tiles_dir}')
    metatile = int(data.get('metatile', 4))
    if not 1 <= metatile <= 16:
        raise ConfigError("'metatile' must be between 1 and 16")
    return Config(full.as_posix(), exe, layers, bbox, max(1, max_parallel), warnings,
                  tiles_dir or None, metatile, crs)
