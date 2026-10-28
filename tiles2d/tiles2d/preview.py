# SPDX-License-Identifier: AGPL-3.0-or-later
"""index.html: a Leaflet page to look at the pyramids right away, without a
server (like the leaflet.html of gdal2tiles). One overlay per layer."""
from __future__ import annotations

import json
from pathlib import Path

PAGE = '''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>tiles2d preview</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>
  html, body, #map {{ height: 100%; margin: 0; background: #ffffff; }}
  #info {{ position: absolute; z-index: 1000; bottom: 8px; left: 8px; padding: 4px 8px;
           font: 12px/1.4 Consolas, Menlo, monospace; background: rgba(255,255,255,.9);
           border: 1px solid #ccc; border-radius: 4px; }}
  .debug-tile {{ outline: 1px solid rgba(220,40,40,.6); font: 11px Consolas, monospace;
                 color: #c22; padding: 2px; box-sizing: border-box; }}
</style>
</head>
<body>
<div id="map"></div>
<div id="info"></div>
<script>
const P = {params};
const xy = (x, y) => L.latLng(y, x);                 // Lambert (X, Y) -> Leaflet [Y, X]
const map = L.map('map', {{ crs: L.CRS.Simple, minZoom: P.zmin, maxZoom: P.zmax,
                            zoomSnap: 1, attributionControl: false }});
const bounds = L.latLngBounds(xy(P.bbox[0], P.bbox[1]), xy(P.bbox[2], P.bbox[3]));
map.fitBounds(bounds);

const overlays = {{}};
for (const l of P.layers) {{
  overlays[l.name] = L.tileLayer(l.path + '/{{z}}/{{x}}/{{y}}.png', {{
    minZoom: P.zmin, maxZoom: P.zmax, bounds: bounds, noWrap: true, keepBuffer: 1,
  }}).addTo(map);
}}
// Debug grid: each tile's border and z/x/y, to check the seams.
const Debug = L.GridLayer.extend({{
  createTile(c) {{ const d = document.createElement('div'); d.className = 'debug-tile';
                  d.textContent = c.z + '/' + c.x + '/' + c.y; return d; }}
}});
overlays['tile grid (debug)'] = new Debug({{ minZoom: P.zmin, maxZoom: P.zmax, bounds: bounds }});
L.control.layers(null, overlays, {{ collapsed: false }}).addTo(map);

const info = document.getElementById('info');
const show = (e) => {{
  const z = map.getZoom(), res = Math.pow(2, -z);
  let s = 'zoom ' + z + ' · ' + res + ' m/px · 1:' + Math.round(res / 0.00028);
  if (e && e.latlng) s += ' · X ' + Math.round(e.latlng.lng) + '  Y ' + Math.round(e.latlng.lat);
  info.textContent = s;
}};
map.on('zoomend', show); map.on('mousemove', show); show();
</script>
</body>
</html>
'''


def write(out_dir: Path, layers: list[tuple[str, str]], bbox, zmin: int, zmax: int) -> Path:
    """layers: (name, relative path of its pyramid). Returns the page path."""
    params = {'bbox': list(bbox), 'zmin': zmin, 'zmax': zmax,
              'layers': [{'name': n, 'path': p} for n, p in layers]}
    page = out_dir / 'index.html'
    page.write_text(PAGE.format(params=json.dumps(params)), encoding='utf-8')
    return page
