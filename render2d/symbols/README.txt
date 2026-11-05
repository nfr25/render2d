render2d's symbol library (render2d notes §76)

A style can name an image without its folder:

    - external: {url: nature/park.svg}

render2d (and carto2D, viewer2d through render2d) looks for it beside the
style first, then here: in the "symbols" folder beside the program.
More folders: render2d --symbols DIR, or the R2D_SYMBOLS variable.

Shipped: 84 symbols in 8 families (nature, transport, health, public,
culture, worship, shops, street), a white glyph on a disc of the family's
colour, 20 x 20. Made by tools/make-symbols.py from Maki and Temaki (CC0,
public domain); SOURCES.txt says which glyph each one is. A style names
them by family: nature/tree.svg, transport/bus-stop.svg.

Your own symbols go in user/ : an update of the program replaces the
symbols it ships and never touches user/. Do not change a shipped symbol
in place (the next update would replace it): copy it into user/ first.

A map sent to someone else is packaged with the symbols it names.
