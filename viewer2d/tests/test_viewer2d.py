# SPDX-License-Identifier: AGPL-3.0-or-later
"""Tests without a browser: configuration and command line.
Run with:  python -m pytest tests   (or simply: python tests/test_viewer2d.py)
"""
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from viewer2d import config                                              # noqa: E402
from viewer2d.render import MIN_RENDER2D, build_command, parse_version   # noqa: E402


def write(tmp: Path, name: str, text: str) -> Path:
    p = tmp / name
    p.write_text(text, encoding='utf-8')
    return p


def make_config(tmp: Path, body: str) -> Path:
    exe = write(tmp, 'render2d.exe', '')
    write(tmp, 'parcelles.shp', '')
    write(tmp, 'parcelles.yaml', '')
    return write(tmp, 'viewer.yaml', body.format(exe=exe.as_posix(), d=tmp.as_posix()))


def test_load_and_command():
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        cfg = config.load(make_config(tmp, '''
render2d: {exe}
layers:
  - source: {d}/parcelles.shp
    style: {d}/parcelles.yaml
  - source: {d}/absent.shp
start:
  bbox: [150000, 160000, 155000, 165000]
'''))
        assert len(cfg.layers) == 2
        assert cfg.start_bbox == (150000.0, 160000.0, 155000.0, 165000.0)
        assert any('absent.shp' in w for w in cfg.warnings)   # a warning, not an error

        # §11: render2d reads the layers itself, --config viewer.yaml.
        cmd = build_command(cfg, (150000, 160000, 155000, 165000), 1000, 800, 'out.png')
        assert cmd[0].endswith('render2d.exe')
        assert cmd[1] == '--config' and cmd[2] == cfg.path and cmd[2].endswith('viewer.yaml')
        assert Path(cmd[2]).is_absolute()
        assert cmd[3] == '--bbox' and cmd[4:8] == ['150000.000', '160000.000', '155000.000', '165000.000']
        assert cmd[-4:] == ['--size', '1000x800', '-o', 'out.png']
        assert not any(a.endswith('parcelles.shp') for a in cmd)   # no more layer by layer

        # A 4K screen scaled to 150 %: the image is seen 1.5 times smaller.
        cmd = build_command(cfg, (150000, 160000, 155000, 165000), 1500, 1200, 'out.png', 1.5)
        assert cmd[-6:] == ['--size', '1500x1200', '--pixel-ratio', '1.5', '-o', 'out.png']


def test_relative_paths():
    """§11, the worked example: relative to the FILE's folder, as render2d (§57)."""
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        sub = tmp / 'viewer2d'
        sub.mkdir()
        exe = write(tmp, 'render2d.exe', '')
        body = (
            f'render2d: {exe.as_posix()}\n'
            'layers:\n'
            '  - source: ../data/parcelles.fgb\n'
            '    style: ../render2d/styles/parcelles.yaml\n'
            '    title: Parcelles\n'
            '  - source: ../data/adresses.gpkg::adresses\n'
            '    visible: false\n'
            '  - source: "PG:service=urbis"\n'
            'start:\n'
            '  bbox: [150000, 160000, 155000, 165000]\n'
            'max_parallel: 2\n'
            'metatile: 4\n')
        cfg = config.load(write(sub, 'viewer.yaml', body))
        root = Path(os.path.normpath(tmp.resolve())).as_posix()
        a, b, c = cfg.layers
        assert a.source == f'{root}/data/parcelles.fgb', a.source
        assert a.style == f'{root}/render2d/styles/parcelles.yaml', a.style
        assert b.source == f'{root}/data/adresses.gpkg::adresses'   # only the file part is a path
        assert c.source == 'PG:service=urbis'                        # not a path at all
        assert a.visible and not b.visible
        assert a.label() == 'Parcelles' and b.label() == 'adresses.gpkg::adresses'
        assert not any('unknown' in w for w in cfg.warnings)         # metatile, max_parallel: known


def test_unknown_keys_and_version():
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        cfg = config.load(make_config(tmp, '''
render2d: {exe}
layer: oops
layers:
  - source: {d}/parcelles.shp
    stile: x.yaml
start:
  bbox: [150000, 160000, 155000, 165000]
'''))
        assert sum('unknown' in w for w in cfg.warnings) == 2   # 'layer', 'stile': typos, said
    assert parse_version('render2d 0.3.8 (step 3h: --config), built Oct 22 2026') == (0, 3, 8)
    assert parse_version('render2d 0.3.7 (step 3g: querying a zone)') < MIN_RENDER2D
    assert parse_version('render2d 0.4.0') > MIN_RENDER2D
    assert parse_version('something else') is None


def test_errors():
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        bad_bbox = make_config(tmp, 'render2d: {exe}\nlayers: [{{source: x.shp}}]\nstart: {{bbox: [5, 0, 1, 1]}}\n')
        (tmp / 'v').mkdir()
        bad_visible = make_config(tmp / 'v', 'render2d: {exe}\nlayers: [{{source: x.shp, visible: maybe}}]\n'
                                       'start: {{bbox: [0, 0, 1, 1]}}\n')
        for path, part in [(bad_bbox, 'xmin < xmax'),
                           (bad_visible, 'true or false'),
                           (write(tmp, 'a.yaml', 'layers: []\n'), 'render2d'),
                           (write(tmp, 'b.yaml', '- not a map\n'), 'expected a map'),
                           (tmp / 'missing.yaml', 'cannot read')]:
            try:
                config.load(path)
                raise AssertionError(f'{path.name}: error expected')
            except config.ConfigError as e:
                assert part in str(e), (path.name, str(e))


if __name__ == '__main__':
    test_load_and_command()
    test_relative_paths()
    test_unknown_keys_and_version()
    test_errors()
    print('viewer2d tests passed')
