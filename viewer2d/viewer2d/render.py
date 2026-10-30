# SPDX-License-Identifier: AGPL-3.0-or-later
"""The /render.png route: one image per view, like a WMS GetMap (notions §2, §3, §6, §7).

The browser sends six numbers; we turn them into a render2d command line,
run it, and send back the image with render2d's report in a header.
"""
from __future__ import annotations

import asyncio
import os
import re
import subprocess
import tempfile
from pathlib import Path
from urllib.parse import quote

from fastapi import Query
from fastapi.responses import Response

from .config import Config

MAX_SIDE = 8192   # pixels; larger requests are refused (a typo, not a map)

# GDAL on MSYS2 brings OpenBLAS, which by default prepares one thread and its
# memory buffers per core in every process. render2d needs one: our
# parallelism is the semaphore (same setting as tiles2d).
RENDER_ENV = {**os.environ, 'OPENBLAS_NUM_THREADS': '1', 'OMP_NUM_THREADS': '1'}


# render2d reads viewer.yaml itself since 0.3.8 (--config, notions §11), and
# converts layers in other systems since 0.4.5 (--crs, render2d notes §70).
MIN_RENDER2D = (0, 4, 5)


def parse_version(line: str) -> tuple[int, int, int] | None:
    """'render2d 0.3.8 (step 3h: --config), built ...' -> (0, 3, 8)."""
    m = re.match(r'render2d (\d+)\.(\d+)\.(\d+)', line)
    return tuple(int(g) for g in m.groups()) if m else None


def check_version(exe: str) -> tuple[bool, str]:
    """Runs `render2d --version` once, at start (§11): (usable, message).
    Better a clear refusal now than a failed render at every move."""
    try:
        proc = subprocess.run([exe, '--version'], capture_output=True, text=True, timeout=20,
                              encoding='utf-8', errors='replace', env=RENDER_ENV)
    except OSError as e:
        return False, f'cannot run {exe}: {e}'
    except subprocess.TimeoutExpired:
        return False, f'{exe} --version did not answer'
    first = ((proc.stdout or proc.stderr).strip().splitlines() or [''])[0]
    found = parse_version(first)
    if found is None:
        return False, f'{exe} --version: unexpected answer {first!r}'
    need = '.'.join(map(str, MIN_RENDER2D))
    if found < MIN_RENDER2D:
        return False, (f'render2d {".".join(map(str, found))} is too old: viewer2d needs {need} or later '
                       f'(--config, --crs). Rebuild it: make release in render2d.')
    return True, first


def build_command(cfg: Config, bbox: tuple[float, float, float, float],
                  width: int, height: int, output: str, ratio: float = 1.0) -> list[str]:
    """The exact render2d command line: shown in the report, so it can be replayed.
    render2d reads the layers and styles in viewer.yaml itself (§11)."""
    cmd = [cfg.render2d, '--config', cfg.path, '--crs', cfg.crs]   # always said (render2d §70)
    cmd += ['--bbox', *(f'{v:.3f}' for v in bbox), '--size', f'{width}x{height}']
    if ratio != 1.0:   # high-density screen: the image is seen `ratio` times smaller (notions §6)
        cmd += ['--pixel-ratio', f'{ratio:g}']
    return cmd + ['-o', output]


def run_render2d(cmd: list[str]) -> tuple[int, str]:
    """Runs render2d and returns (exit code, report). Called in a worker thread:
    subprocess.run works on every platform and event loop (Windows included)."""
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace',
                          env=RENDER_ENV)
    report = (proc.stdout + proc.stderr).strip()
    return proc.returncode, report


def register(app, cfg: Config) -> None:
    slots = asyncio.Semaphore(cfg.max_parallel)   # notions §2, §3: not 20 processes at once

    @app.get('/render.png')
    async def render(xmin: float, ymin: float, xmax: float, ymax: float,
                     w: int = Query(gt=0, le=MAX_SIDE), h: int = Query(gt=0, le=MAX_SIDE),
                     ratio: float = Query(1.0, gt=0, le=8)):
        if not (xmin < xmax and ymin < ymax):
            return Response('xmin < xmax and ymin < ymax expected', status_code=400, media_type='text/plain')

        with tempfile.TemporaryDirectory(prefix='viewer2d-') as tmp:
            out = str(Path(tmp) / 'view.png')
            cmd = build_command(cfg, (xmin, ymin, xmax, ymax), w, h, out, ratio)
            async with slots:
                code, report = await asyncio.to_thread(run_render2d, cmd)

            # The replayable command, without the temporary output path.
            shown = ' '.join(f'"{a}"' if ' ' in a else a for a in cmd[:-1]) + ' map.png'
            text = report.replace(out, 'map.png') + f'\n\ncommand: {shown}'
            png = Path(out)
            if code != 0 or not png.is_file():
                # notions §7: the error is shown in the page, the previous image stays.
                return Response(text, status_code=502, media_type='text/plain')
            return Response(png.read_bytes(), media_type='image/png',
                            headers={'X-Render2d-Report': quote(text),
                                     'Cache-Control': 'no-store'})
