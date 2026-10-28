# render2d

A small map renderer in the spirit of the GDAL tools, and its web test bench.

```
render2d/    the C++ renderer: vector and raster layers in (GDAL), a map out
             (PNG, SVG, PDF, drawn with Cairo); YSLD styles, CQL filters, labels,
             queries; a command line and a set of libraries
viewer2d/    a test bench in the browser (NiceGUI + Leaflet): pan and zoom,
             render2d draws each view; tile mode with tiles2d
```

Each project has its own README (build, run) and its design notes,
`docs/notions.en.html`: the reference, with the maths worked out step by step.
The code follows them.

## Quick start (Windows, MSYS2 UCRT64)

```
cd render2d
make fresh                        # Debug + tests, then Release (build-release/)
cd ../viewer2d
copy viewer.example.yaml viewer.yaml     # then adapt the paths
python -m viewer2d
```

## License

Copyright (C) 2026 Noël Frankinet.

render2d and viewer2d are free software: you can redistribute them and/or
modify them under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version. See [`LICENSE`](LICENSE).

They are distributed in the hope that they will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.

For other licensing terms (for example, use in a closed product or service),
contact the author.

Contributions: please open an issue first. Code contributions can only be
accepted with a contributor agreement that keeps the licensing in one hand.
