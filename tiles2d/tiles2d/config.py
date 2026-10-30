# SPDX-License-Identifier: AGPL-3.0-or-later
"""The map file: the same format as viewer2d's viewer.yaml, so one file
describes a map for both tools.

    render2d: C:/.../render2d.exe
    layers:
      - source: C:/data/parcelles.shp
        style:  C:/.../styles/parcelles.yaml
        crs:    EPSG:31300                 # optional: the file's system, when it says none
    crs: EPSG:31370                        # optional: the tiles' system, in metres (default)
    start:
      bbox: [xmin, ymin, xmax, ymax]     # default area for tiles2d
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import yaml


class ConfigError(Exception):
    """The map file cannot be used."""


# The tiles' system when the map file names none: Lambert 72 (render2d §70).
DEFAULT_CRS = 'EPSG:31370'


@dataclass
class Layer:
    source: str
    style: str | None
    name: str                      # folder name of its pyramid, from the source file name
    crs: str | None = None         # its system, declared in the map file ("" / None: the file's)
    map_crs: str = DEFAULT_CRS     # the system the tiles are drawn in (render2d --crs)


@dataclass
class MapConfig:
    render2d: str
    layers: list[Layer]
    bbox: tuple[float, float, float, float] | None
    crs: str = DEFAULT_CRS


def layer_name(source: str, taken: set[str]) -> str:
    """Folder name of a layer's pyramid, from its source file name. Shared with viewer2d."""
    base = source.split('::')[-1] if '::' in source else Path(source.replace('\\', '/')).stem
    base = ''.join(c if c.isalnum() or c in '-_' else '_' for c in base) or 'layer'
    name, k = base, 2
    while name in taken:                 # two layers from files with the same name
        name, k = f'{base}_{k}', k + 1
    taken.add(name)
    return name


def load(path: str | Path) -> MapConfig:
    path = Path(path)
    try:
        data = yaml.safe_load(path.read_text(encoding='utf-8'))
    except OSError as e:
        raise ConfigError(f'cannot read {path}: {e}') from None
    except yaml.YAMLError as e:
        raise ConfigError(f'{path} is not valid YAML: {e}') from None
    if not isinstance(data, dict):
        raise ConfigError(f'{path}: expected a map (render2d, layers, start)')

    exe = data.get('render2d')
    if not exe or not Path(exe).is_file():
        raise ConfigError(f"'render2d': executable not found: {exe}")

    crs = str(data.get('crs') or DEFAULT_CRS)
    taken: set[str] = set()
    layers = []
    for i, item in enumerate(data.get('layers') or []):
        if not isinstance(item, dict) or 'source' not in item:
            raise ConfigError(f"layers[{i}]: expected 'source' (and optionally 'style')")
        src = str(item['source'])
        layers.append(Layer(src, str(item['style']) if item.get('style') else None, layer_name(src, taken),
                            str(item['crs']) if item.get('crs') else None, crs))
    if not layers:
        raise ConfigError("'layers': at least one layer is needed")

    bbox = None
    start = data.get('start') or {}
    if 'bbox' in start:
        try:
            bbox = tuple(float(v) for v in start['bbox'])
            assert len(bbox) == 4 and bbox[0] < bbox[2] and bbox[1] < bbox[3]
        except (TypeError, ValueError, AssertionError):
            raise ConfigError('start.bbox: expected [xmin, ymin, xmax, ymax]') from None
    return MapConfig(str(exe), layers, bbox, crs)
