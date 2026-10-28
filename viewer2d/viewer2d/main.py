# SPDX-License-Identifier: AGPL-3.0-or-later
"""viewer2d: a test bench for render2d (notions §1).

    python -m viewer2d                 # reads viewer.yaml in the current folder
    python -m viewer2d --config my.yaml --port 8081
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from nicegui import app, ui

from . import __version__, config, page, render

STATIC = Path(__file__).resolve().parent.parent / 'static'


def main() -> None:
    parser = argparse.ArgumentParser(prog='viewer2d', description='Test bench for render2d.')
    parser.add_argument('--config', default='viewer.yaml', help='configuration file (default: viewer.yaml)')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--no-browser', action='store_true', help='do not open a browser tab')
    args = parser.parse_args()

    try:
        cfg = config.load(args.config)
    except config.ConfigError as e:
        sys.exit(f'viewer2d: {e}')
    for w in cfg.warnings:
        print(f'warning: {w}')
    ok, message = render.check_version(cfg.render2d)   # §11: render2d 0.3.8 or later
    if not ok:
        sys.exit(f'viewer2d: {message}')
    print(f'viewer2d {__version__}, {message}')

    render.register(app, cfg)
    tile_service = None
    if cfg.tiles_dir:                              # tile mode: needs tiles2d (notions §10)
        from . import tiles
        tile_service = tiles.register(app, cfg)
    page.register(cfg, tile_service)
    app.add_static_files('/static', str(STATIC))
    ui.run(title='viewer2d', port=args.port, reload=False, show=not args.no_browser)
