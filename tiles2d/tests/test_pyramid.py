# SPDX-License-Identifier: AGPL-3.0-or-later
"""Cutting, cache key, and (when render2d is available) seams.

    python tests/test_pyramid.py
    RENDER2D=/path/to/render2d SHP=/path/to/buildings.shp STYLE=/path/to/style.yaml python tests/test_pyramid.py

The seam test renders two tiles on each side of a metatile border, stitches
them, and compares with one direct render2d call over the same area.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from PIL import Image, ImageChops  # noqa: E402

from tiles2d import cache, grid  # noqa: E402
from tiles2d.config import Layer  # noqa: E402
from tiles2d.pyramid import build_layer, cut_metatile, tile_path  # noqa: E402


def test_cut_metatile():
    # A 512 x 512 metatile (n = 2): top-left quarter red, rest transparent.
    img = Image.new('RGBA', (512, 512), (0, 0, 0, 0))
    img.paste((255, 0, 0, 255), (0, 0, 256, 256))
    with tempfile.TemporaryDirectory() as t:
        root = Path(t)
        written, empty = cut_metatile(img, root, 0, 10, -20, 2)
        assert (written, empty) == (4, 3)
        # Metatile (10, -20) with n = 2 holds tiles 20..21 x -40..-39; (20, -40) is top-left.
        red = Image.open(tile_path(root, 0, 20, -40)).convert('RGBA')
        assert red.getpixel((128, 128)) == (255, 0, 0, 255)
        assert Image.open(tile_path(root, 0, 21, -39)).getchannel('A').getbbox() is None


def test_cache_key():
    with tempfile.TemporaryDirectory() as t:
        d = Path(t)
        exe = d / 'render2d.exe'; exe.write_bytes(b'x')
        shp = d / 'b.shp'; shp.write_bytes(b'shape')
        style = d / 's.yaml'; style.write_text('name: a\n')
        layer = Layer(str(shp), str(style), 'b')
        k1 = cache.layer_key(layer, str(exe))
        assert len(k1) == 12 and k1 == cache.layer_key(layer, str(exe))   # stable
        style.write_text('name: b\n')
        k2 = cache.layer_key(layer, str(exe))
        assert k2 != k1                                                    # style edited
        (d / 'b.dbf').write_bytes(b'attributes')
        assert cache.layer_key(layer, str(exe)) != k2                      # companion file added


def test_seam_with_render2d():
    exe, shp, style = os.environ.get('RENDER2D'), os.environ.get('SHP'), os.environ.get('STYLE')
    if not (exe and shp):
        print('  (seam test skipped: set RENDER2D and SHP)')
        return
    layer = Layer(shp, style, 'test')
    with tempfile.TemporaryDirectory() as t:
        root = Path(t) / 'pyr'
        # Data extent centre, then tiles 4k-1 | 4k at zoom 0: two different metatiles.
        info = subprocess.run([exe, shp, '--size', '10', '-o', str(Path(t) / 'probe.png')],
                              capture_output=True, text=True)
        assert info.returncode == 0, info.stdout + info.stderr
        # Use a fixed area inside the synthetic commune of the docs if available,
        # otherwise the first tile column multiple of 4 near x = 150 000.
        cx, cy = float(os.environ.get('CX', 150500)), float(os.environ.get('CY', 166200))
        tx, ty = grid.tile_of(cx, cy, 0)
        tx -= tx % 4                       # tile tx starts a metatile, tx-1 ends the previous one
        a, b = grid.tile_bbox(0, tx - 1, ty), grid.tile_bbox(0, tx, ty + 1)
        area = (a[0], b[1], b[2], a[3])
        build_layer(exe, layer, 'k', root, area, range(0, 1), 4, 2, False,
                    lambda *x: None, lambda zs: None)
        stitched = Image.new('RGBA', (512, 512))
        for i, x in enumerate((tx - 1, tx)):
            for j, y in enumerate((ty, ty + 1)):
                stitched.paste(Image.open(tile_path(root, 0, x, y)).convert('RGBA'), (i * 256, j * 256))
        direct = Path(t) / 'direct.png'
        cmd = [exe, shp] + (['--style', style] if style else []) + \
              ['--bbox', *map(str, area), '--size', '512x512', '--background', 'none', '-o', str(direct)]
        subprocess.run(cmd, check=True, capture_output=True)
        diff = ImageChops.difference(stitched, Image.open(direct).convert('RGBA'))
        worst = max(band.getextrema()[1] for band in diff.split())
        assert worst <= 2, f'seam: max channel difference {worst}'
        print(f'  seam test: max channel difference {worst} (<= 2 expected)')


if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_'):
            fn()
    print('pyramid tests passed')
