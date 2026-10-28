# SPDX-License-Identifier: AGPL-3.0-or-later
"""The page: a map area, the mode switch and the report (notions §1, §4, §7, §10).

The map itself is plain Leaflet in static/viewer.js: NiceGUI's ui.leaflet
cannot use L.CRS.Simple (notions §4). NiceGUI builds the page around it.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import TYPE_CHECKING

from nicegui import ui

from .config import Config

if TYPE_CHECKING:                 # only for the type hint: tiles2d is not needed without tile mode
    from .tiles import TileService

LEAFLET = 'https://unpkg.com/leaflet@1.9.4/dist'
VIEWER_JS = Path(__file__).resolve().parent.parent / 'static' / 'viewer.js'


def register(cfg: Config, tile_service: TileService | None) -> None:

    @ui.page('/')
    async def index():
        ui.add_head_html(f'<link rel="stylesheet" href="{LEAFLET}/leaflet.css">')
        ui.add_head_html(f'<script src="{LEAFLET}/leaflet.js"></script>')
        # ?v=<date of the file>: a new viewer.js is always fetched, never an old cached copy.
        ui.add_head_html(f'<script src="/static/viewer.js?v={int(VIEWER_JS.stat().st_mtime)}"></script>')
        ui.add_head_html('''<style>
            .viewer2d-map { background: #ffffff; border: 1px solid #ddd; }
            .viewer2d-report { font: 12px/1.45 Consolas, Menlo, monospace; white-space: pre-wrap;
                               margin: 0; max-height: 180px; overflow: auto; }
        </style>''')

        with ui.column().classes('w-full h-screen p-2 gap-2'):
            with ui.row().classes('w-full items-center gap-4'):
                ui.label('viewer2d').classes('text-lg font-bold')
                if tile_service:
                    # A NiceGUI control driving the JavaScript map: Python on_change -> JS call.
                    ui.toggle({'view': 'Vue', 'tiles': 'Tuiles'}, value='view',
                              on_change=lambda e: ui.run_javascript(f"viewer2dSetMode('{e.value}')")) \
                        .props('dense no-caps')
                ui.label(' · '.join(l.label() for l in cfg.layers)) \
                    .classes('text-sm text-gray-600')
                ui.element('span').classes('viewer2d-cursor text-sm text-gray-600 ml-auto')
            ui.element('div').classes('viewer2d-map w-full grow').style('min-height: 300px')
            ui.element('pre').classes('viewer2d-report w-full')   # filled by viewer.js

        # The map can only be created once the page exists in the browser.
        await ui.context.client.connected()
        # §11: tile layers shown by title; those with visible: false unchecked at start.
        settings = {'bbox': list(cfg.start_bbox), 'warnings': cfg.warnings,
                    'tiles': ({'layers': [l.name for l in tile_service.layers],
                               'titles': {t.name: c.label() for t, c in zip(tile_service.layers, cfg.layers)},
                               'hidden': [t.name for t, c in zip(tile_service.layers, cfg.layers) if not c.visible],
                               'root': str(tile_service.root)} if tile_service else None)}
        ui.run_javascript(f'viewer2dInit({json.dumps(settings)})')
