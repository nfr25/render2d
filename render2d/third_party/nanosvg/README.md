# nanosvg

`nanosvg.h`, unchanged, from https://github.com/memononen/nanosvg
(`src/nanosvg.h`, commit 239e102ec2c691f2902e20ace2ed36ee4a35cfe6).

Copyright (c) 2013-14 Mikko Mononen. zlib licence (the text is at the top of
the file). Its SVG parser is based on Anti-Grain Geometry 2.4's SVG example,
by Maxim Shemanarev.

render2d uses it to read SVG symbols as paths and draw them with its own
canvas, so they stay vector in a PDF (render2d notes §75). One header, no
library, no DLL.
