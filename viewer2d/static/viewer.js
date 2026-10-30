// SPDX-License-Identifier: AGPL-3.0-or-later
// viewer2d: Leaflet in the map's metres (L.CRS.Simple: Lambert 72 by default,
// any system in metres with crs: in viewer.yaml, render2d §70), two modes:
//   view  - one render2d image per view (notions §4 to §7)
//   tiles - the pyramids of the shared cache, filled on demand (notions §10)
'use strict';

window.viewer2dInit = function (settings) {
  const mapEl    = document.querySelector('.viewer2d-map');
  const reportEl = document.querySelector('.viewer2d-report');
  const cursorEl = document.querySelector('.viewer2d-cursor');

  // §4: we think in (X, Y), Leaflet wants [lat, lng] = [Y, X].
  const xy = (x, y) => L.latLng(y, x);
  const fmt = (v, d = 0) => v.toLocaleString('fr-BE', { minimumFractionDigits: d, maximumFractionDigits: d });

  // §4, §5: 1 unit = 1 metre = 1 pixel at zoom 0; negative zooms for a whole
  // region, fractional zooms (view mode only) so the wheel does not jump.
  const ZMIN = -7, ZMAX = 4;
  const map = L.map(mapEl, {
    crs: L.CRS.Simple, minZoom: ZMIN, maxZoom: ZMAX,
    zoomSnap: 0, zoomDelta: 0.5, wheelPxPerZoomLevel: 120, attributionControl: false,
  });
  const [x0, y0, x1, y1] = settings.bbox;
  map.fitBounds(L.latLngBounds(xy(x0, y0), xy(x1, y1)));

  map.on('mousemove', (e) => {
    if (cursorEl) cursorEl.textContent = `X ${fmt(e.latlng.lng)}   Y ${fmt(e.latlng.lat)}`;
  });

  const configNote = (settings.warnings || []).length
    ? 'configuration: ' + settings.warnings.join('; ') + '\n' : '';
  const scaleText = (res) => `${fmt(res, 3)} m/px · 1:${fmt(Math.round(res / 0.00028))}`;

  let mode = 'view';

  // =====================================================================
  // View mode: one image per view
  // =====================================================================
  let seq = 0;          // §6: number of the last request sent
  let shown = null;     // {id, layer, url} of the image on the map
  let timer = null;

  function header(q, ratio) {
    const res = (q.xmax - q.xmin) / q.w * ratio;   // metres per pixel seen
    let s = `view: zoom ${fmt(map.getZoom(), 2)} · ${q.w}×${q.h} px · ${scaleText(res)}`;
    if (ratio !== 1) s += ` (×${ratio} high-density screen: render2d draws ${ratio}× larger, --pixel-ratio)`;
    return configNote + s + '\n';
  }

  async function request() {
    const b = map.getBounds(), size = map.getSize();
    const ratio = window.devicePixelRatio || 1;
    const q = {
      xmin: b.getWest(), ymin: b.getSouth(), xmax: b.getEast(), ymax: b.getNorth(),
      w: Math.round(size.x * ratio), h: Math.round(size.y * ratio),
      ratio,
    };
    const id = ++seq;
    const bounds = L.latLngBounds(xy(q.xmin, q.ymin), xy(q.xmax, q.ymax));
    try {
      const resp = await fetch('/render.png?' + new URLSearchParams(q));
      if (id !== seq || mode !== 'view') return;     // a newer view, or the mode changed
      if (!resp.ok) {                                // §7: show the error, keep the old image
        reportEl.textContent = header(q, ratio) + 'render2d failed:\n' + await resp.text();
        return;
      }
      const report = decodeURIComponent(resp.headers.get('X-Render2d-Report') || '');
      const url = URL.createObjectURL(await resp.blob());
      if (id !== seq || mode !== 'view') { URL.revokeObjectURL(url); return; }

      // §6: no flicker: the old image is removed only once the new one is loaded.
      const layer = L.imageOverlay(url, bounds);
      layer.once('load', () => {
        if (mode !== 'view' || (shown && shown.id > id)) { map.removeLayer(layer); URL.revokeObjectURL(url); return; }
        if (shown) { map.removeLayer(shown.layer); URL.revokeObjectURL(shown.url); }
        shown = { id, layer, url };
      });
      layer.addTo(map);
      reportEl.textContent = header(q, ratio) + report;
    } catch (err) {
      if (id === seq) reportEl.textContent = 'request failed: ' + err;
    }
  }

  function clearView() {
    seq++;                                            // forget requests in flight
    if (shown) { map.removeLayer(shown.layer); URL.revokeObjectURL(shown.url); shown = null; }
  }

  // =====================================================================
  // Tiles mode: one tile layer per data layer, from the shared cache
  // =====================================================================
  const tiles = settings.tiles;                     // null when viewer.yaml has no 'tiles:'
  const tileLayers = {};                            // name -> L.tileLayer
  const keys = {};                                  // name -> current cache key
  let control = null, poll = null, lastStatus = null, lastInfo = '';

  const tileUrl = (name, key) => `/tiles/${encodeURIComponent(name)}/${key}/{z}/{x}/{y}.png`;

  // Debug grid: each tile's border and z/x/y, to check the seams.
  const Debug = L.GridLayer.extend({
    createTile(c) {
      const d = document.createElement('div');
      d.style.cssText = 'outline:1px solid rgba(220,40,40,.6);font:11px Consolas,monospace;color:#c22;padding:2px;box-sizing:border-box';
      d.textContent = c.z + '/' + c.x + '/' + c.y;
      return d;
    },
  });
  const debugLayer = new Debug({ minZoom: ZMIN, maxZoom: ZMAX });

  function tilesReport() {
    const z = map.getZoom();
    let s = configNote + `tiles: zoom ${z} · ${scaleText(Math.pow(2, -z))} · cache ${tiles.root}\n`;
    if (lastStatus) {
      const st = lastStatus.stats;
      for (const l of lastStatus.layers) s += `  ${l.name}  key ${l.key}\n`;
      s += `served ${st.served} tiles · drawn on demand ${st.rendered_metatiles} metatiles`;
      if (st.rendered_metatiles) s += ` (${fmt(1000 * st.render_seconds / st.rendered_metatiles, 0)} ms each)`;
      s += ` · errors ${st.errors}\n`;
      if (st.last_error) s += `last error: ${st.last_error}\n`;
    }
    if (lastInfo) s += '\n' + lastInfo;
    else s += '\nclick on the map: tile, metatile and the render2d command that draws it';
    reportEl.textContent = s;
  }

  // Every 2 s: current keys (a style edit changes its key) and counters.
  async function refresh() {
    try {
      const st = await (await fetch('/tiles/status')).json();
      lastStatus = st;
      for (const l of st.layers) {
        if (keys[l.name] !== l.key) {
          keys[l.name] = l.key;
          if (tileLayers[l.name]) tileLayers[l.name].setUrl(tileUrl(l.name, l.key));   // redraws
        }
      }
      if (mode === 'tiles') tilesReport();
    } catch (err) { /* server restarting: try again at the next tick */ }
  }

  async function enterTiles() {
    clearView();
    // Tiles exist only at whole zoom levels: what you see is what was drawn.
    map.options.zoomSnap = 1; map.options.zoomDelta = 1;
    map.setZoom(Math.round(map.getZoom()));
    await refresh();
    const overlays = {};
    for (const name of tiles.layers) {
      if (!tileLayers[name]) {
        tileLayers[name] = L.tileLayer(tileUrl(name, keys[name]), { minZoom: ZMIN, maxZoom: ZMAX, keepBuffer: 2 });
      }
      // viewer.yaml: visible: false -> unchecked at start; the title, if any (notions §11).
      if (!(tiles.hidden || []).includes(name)) tileLayers[name].addTo(map);
      overlays[(tiles.titles || {})[name] || name] = tileLayers[name];
    }
    overlays['tile grid (debug)'] = debugLayer;
    control = L.control.layers(null, overlays, { collapsed: false }).addTo(map);
    poll = setInterval(refresh, 2000);
    tilesReport();
  }

  function leaveTiles() {
    clearInterval(poll); poll = null;
    if (control) { control.remove(); control = null; }
    for (const l of Object.values(tileLayers)) map.removeLayer(l);
    map.removeLayer(debugLayer);
    map.options.zoomSnap = 0; map.options.zoomDelta = 0.5;
    lastInfo = '';
  }

  map.on('click', async (e) => {
    if (mode !== 'tiles') return;
    const q = { x: e.latlng.lng, y: e.latlng.lat, z: map.getZoom() };
    try {
      const i = await (await fetch('/tiles/info?' + new URLSearchParams(q))).json();
      const b = i.bbox.map(v => fmt(v, 1)).join(' ');
      let s = `tile ${i.tile} · bbox ${b}\nmetatile ${i.metatile} (${i.size}×${i.size} px)\n`;
      for (const [name, cached] of Object.entries(i.cached)) s += `  ${name}: ${cached ? 'in cache' : 'not drawn yet'}\n`;
      for (const c of i.commands) s += `\n${c}`;
      lastInfo = s;
      tilesReport();
    } catch (err) { lastInfo = 'info failed: ' + err; tilesReport(); }
  });
  map.on('zoomend', () => { if (mode === 'tiles') tilesReport(); });

  // =====================================================================
  // Switching
  // =====================================================================
  window.viewer2dSetMode = async (m) => {
    if (m === mode || (m === 'tiles' && !tiles)) return;
    if (mode === 'tiles') leaveTiles();
    mode = m;
    if (m === 'tiles') await enterTiles();
    else request();
  };

  // After each move or zoom, wait a little: a quick series of moves gives one request.
  map.on('moveend', () => {
    if (mode !== 'view') return;
    clearTimeout(timer); timer = setTimeout(request, 120);
  });
  request();
};
