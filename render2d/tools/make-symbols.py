#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Builds render2d's symbol library (notions §76) from Maki and Temaki.

Maki (Mapbox) and Temaki (Rapid editor) are map icons in the public domain
(CC0 1.0): one black glyph per file, 15 x 15 (some Temaki 50 x 50). A black
glyph alone reads badly on a map; the custom of maps is a white glyph on a
disc of a colour per family (green for nature, blue for transport...). This
script makes, for each glyph of the list below, symbols/<family>/<name>.svg:

    a disc of 20 x 20 units, the family's colour, a white rim (readable on an
    orthophoto too), the glyph in white, 11 units, in the middle.

The result is committed with render2d: building render2d never needs the
network. To run it again (a glyph added, a colour changed):

    git clone --depth 1 https://github.com/mapbox/maki.git
    git clone --depth 1 https://github.com/rapideditor/temaki.git
    python tools/make-symbols.py --maki maki --temaki temaki --out symbols

Only the shipped families' folders are rewritten; symbols/user/ is never
touched. symbols/SOURCES.txt lists each symbol, its source and licence.
"""

import argparse
import pathlib
import re
import sys

# The families: folder, colour (#RRGGBB), what they hold.
FAMILIES = {
    "nature":    ("#2F7D4F", "trees, parks, gardens, water"),
    "transport": ("#2B6C8F", "public transport, cycling, cars, parking"),
    "health":    ("#C0392B", "health, emergency, safety"),
    "public":    ("#A0522D", "administration, education, public services"),
    "culture":   ("#7B4FA0", "culture, leisure, sport, heritage"),
    "worship":   ("#5E5E7A", "places of worship, cemeteries"),
    "shops":     ("#C77C02", "shops, food and drink, banks"),
    "street":    ("#666666", "street furniture"),
}

# (family, name in the library, source, glyph in the source)
SYMBOLS = [
    ("nature", "tree",             "temaki", "tree_broadleaved"),
    ("nature", "tree-conifer",     "temaki", "tree_needleleaved"),
    ("nature", "park",             "maki",   "park"),
    ("nature", "garden",           "maki",   "garden"),
    ("nature", "playground",       "maki",   "playground"),
    ("nature", "dog-park",         "maki",   "dog-park"),
    ("nature", "picnic-site",      "maki",   "picnic-site"),
    ("nature", "water",            "maki",   "water"),
    ("nature", "fountain",         "temaki", "fountain"),
    ("nature", "viewpoint",        "maki",   "viewpoint"),

    ("transport", "bus",              "maki",   "bus"),
    ("transport", "tram",             "temaki", "tram"),
    ("transport", "rail",             "maki",   "rail"),
    ("transport", "metro",            "maki",   "rail-metro"),
    ("transport", "bicycle",          "maki",   "bicycle"),
    ("transport", "bicycle-share",    "maki",   "bicycle-share"),
    ("transport", "bicycle-parking",  "temaki", "bicycle_parked"),
    ("transport", "car",              "maki",   "car"),
    ("transport", "parking",          "maki",   "parking"),
    ("transport", "parking-garage",   "maki",   "parking-garage"),
    ("transport", "fuel",             "maki",   "fuel"),
    ("transport", "charging-station", "maki",   "charging-station"),
    ("transport", "taxi",             "maki",   "taxi"),
    ("transport", "airport",          "maki",   "airport"),
    ("transport", "ferry",            "maki",   "ferry"),
    ("transport", "traffic-signals",  "temaki", "traffic_signals"),
    ("transport", "bus-stop",         "temaki", "board_bus"),

    ("health", "hospital",        "maki",   "hospital"),
    ("health", "doctor",          "maki",   "doctor"),
    ("health", "pharmacy",        "maki",   "pharmacy"),
    ("health", "dentist",         "maki",   "dentist"),
    ("health", "veterinary",      "maki",   "veterinary"),
    ("health", "defibrillator",   "maki",   "defibrillator"),
    ("health", "fire-station",    "maki",   "fire-station"),
    ("health", "fire-hydrant",    "temaki", "fire_hydrant"),
    ("health", "police",          "maki",   "police"),
    ("health", "emergency-phone", "maki",   "emergency-phone"),

    ("public", "town-hall",      "maki",   "town-hall"),
    ("public", "post",           "maki",   "post"),
    ("public", "post-box",       "temaki", "post_box"),
    ("public", "school",         "maki",   "school"),
    ("public", "college",        "maki",   "college"),
    ("public", "library",        "maki",   "library"),
    ("public", "information",    "maki",   "information"),
    ("public", "embassy",        "maki",   "embassy"),
    ("public", "toilet",         "maki",   "toilet"),
    ("public", "drinking-water", "maki",   "drinking-water"),
    ("public", "wheelchair",     "maki",   "wheelchair"),

    ("culture", "museum",      "maki",   "museum"),
    ("culture", "theatre",     "maki",   "theatre"),
    ("culture", "cinema",      "maki",   "cinema"),
    ("culture", "art-gallery", "maki",   "art-gallery"),
    ("culture", "music",       "maki",   "music"),
    ("culture", "monument",    "maki",   "monument"),
    ("culture", "statue",      "temaki", "statue"),
    ("culture", "castle",      "maki",   "castle"),
    ("culture", "attraction",  "maki",   "attraction"),
    ("culture", "stadium",     "maki",   "stadium"),
    ("culture", "swimming",    "maki",   "swimming"),
    ("culture", "zoo",         "maki",   "zoo"),

    ("worship", "place-of-worship", "maki", "place-of-worship"),
    ("worship", "christian",        "maki", "religious-christian"),
    ("worship", "jewish",           "maki", "religious-jewish"),
    ("worship", "muslim",           "maki", "religious-muslim"),
    ("worship", "cemetery",         "maki", "cemetery"),

    ("shops", "shop",        "maki",   "shop"),
    ("shops", "grocery",     "maki",   "grocery"),
    ("shops", "bakery",      "maki",   "bakery"),
    ("shops", "restaurant",  "maki",   "restaurant"),
    ("shops", "cafe",        "maki",   "cafe"),
    ("shops", "bar",         "maki",   "bar"),
    ("shops", "fast-food",   "maki",   "fast-food"),
    ("shops", "bank",        "maki",   "bank"),
    ("shops", "atm",         "temaki", "atm"),
    ("shops", "hairdresser", "maki",   "hairdresser"),

    ("street", "bench",           "temaki", "bench"),
    ("street", "street-lamp",     "temaki", "street_lamp_arm"),
    ("street", "waste-basket",    "maki",   "waste-basket"),
    ("street", "recycling",       "maki",   "recycling"),
    ("street", "bollard",         "temaki", "bollard"),
    ("street", "security-camera", "temaki", "security_camera"),
    ("street", "manhole",         "temaki", "manhole"),
    ("street", "clock",           "temaki", "clock"),
    ("street", "telephone",       "maki",   "telephone"),
]

SOURCES = {
    "maki":   ("Maki, by Mapbox", "https://github.com/mapbox/maki"),
    "temaki": ("Temaki, by the Rapid editor's team", "https://github.com/rapideditor/temaki"),
}

SIZE = 20.0      # the symbol: 20 x 20 units, 20 px when the style gives no size
RADIUS = 9.25    # the disc, inside its rim
RIM = 1.5        # the white rim's width
GLYPH = 11.0     # the glyph's box, in the middle


def glyph(path: pathlib.Path):
    """The glyph's inner SVG and its viewBox width and height."""
    text = path.read_text(encoding="utf-8")
    box = re.search(r'viewBox="\s*([-\d.]+)[\s,]+([-\d.]+)[\s,]+([-\d.]+)[\s,]+([-\d.]+)\s*"', text)
    if not box:
        raise ValueError(f"{path}: no viewBox")
    x, y, w, h = (float(v) for v in box.groups())
    inner = re.search(r"<svg[^>]*>(.*)</svg>", text, re.S)
    if not inner:
        raise ValueError(f"{path}: no <svg> element")
    body = inner.group(1)
    body = re.sub(r"<title>.*?</title>|<desc>.*?</desc>|<!--.*?-->", "", body, flags=re.S)
    # The glyph takes the group's white: its own fills go (but "none" stays).
    body = re.sub(r'\sfill="(?!none)[^"]*"', "", body)
    body = re.sub(r'\sid="[^"]*"', "", body)
    return body.strip(), x, y, w, h


def symbol(colour: str, body: str, x: float, y: float, w: float, h: float) -> str:
    k = GLYPH / max(w, h)
    tx = (SIZE - w * k) / 2 - x * k
    ty = (SIZE - h * k) / 2 - y * k
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{SIZE:g}" height="{SIZE:g}" viewBox="0 0 {SIZE:g} {SIZE:g}">\n'
        f'  <circle cx="{SIZE / 2:g}" cy="{SIZE / 2:g}" r="{RADIUS:g}" fill="{colour}" stroke="#FFFFFF" stroke-width="{RIM:g}"/>\n'
        f'  <g fill="#FFFFFF" transform="translate({tx:.4f} {ty:.4f}) scale({k:.6f})">\n'
        f'    {body}\n'
        '  </g>\n'
        '</svg>\n'
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--maki", required=True, type=pathlib.Path, help="a clone of github.com/mapbox/maki")
    ap.add_argument("--temaki", required=True, type=pathlib.Path, help="a clone of github.com/rapideditor/temaki")
    ap.add_argument("--out", required=True, type=pathlib.Path, help="render2d's symbols/ folder")
    a = ap.parse_args()
    roots = {"maki": a.maki / "icons", "temaki": a.temaki / "icons"}

    errors = 0
    lines = []
    for family in FAMILIES:   # rewritten whole; user/ is not a family
        folder = a.out / family
        folder.mkdir(parents=True, exist_ok=True)
        for old in folder.glob("*.svg"):
            old.unlink()
    for family, name, source, src in SYMBOLS:
        colour = FAMILIES[family][0]
        path = roots[source] / f"{src}.svg"
        try:
            body, x, y, w, h = glyph(path)
        except (OSError, ValueError) as e:
            print(f"error: {e}", file=sys.stderr)
            errors += 1
            continue
        (a.out / family / f"{name}.svg").write_text(symbol(colour, body, x, y, w, h), encoding="utf-8")
        lines.append(f"{family}/{name}.svg".ljust(34) + f"{source}: {src}")

    head = [
        "render2d's symbol library: what each symbol is made from (tools/make-symbols.py).",
        "",
        "Each symbol is a white glyph on a disc of its family's colour. The glyphs:",
    ]
    for key, (title, url) in SOURCES.items():
        head.append(f"  {key}: {title}, {url}, CC0 1.0 (public domain).")
    head += ["", "Families:"]
    for family, (colour, what) in FAMILIES.items():
        head.append(f"  {family.ljust(10)} {colour}  {what}")
    head += ["", "Symbols:"]
    (a.out / "SOURCES.txt").write_text("\n".join(head + lines) + "\n", encoding="utf-8")
    print(f"{len(lines)} symbols in {len(FAMILIES)} families written to {a.out}" + (f", {errors} errors" if errors else ""))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
