#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
# package-windows.sh — a render2d that runs on a Windows PC without MSYS2
# (notions §68). To run in the MSYS2 UCRT64 terminal, from anywhere:
#
#     tools/package-windows.sh
#
# 1. builds the Release (make release)
# 2. copies render2d.exe and every DLL it needs from /ucrt64/bin
# 3. copies GDAL's and PROJ's reference files into share/ (render2d finds
#    them beside the executable since 0.4.4)
# 4. copies the styles, README, LICENSE, and the licenses of the libraries
# 5. checks that it starts WITHOUT MSYS2 in the PATH, and draws a test map
# 6. zips it: dist/render2d-<version>-windows-x64.zip
#
# The zip is then attached to a GitHub Release by hand (Releases > Draft a
# new release > the tag > drop the zip).
# =============================================================================

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

prefix="${MINGW_PREFIX:-/ucrt64}"
if [[ ! -d "$prefix/bin" ]]; then
    echo "MSYS2 UCRT64 not found ($prefix): run this in the 'MSYS2 UCRT64' terminal" >&2
    exit 2
fi

version=$(grep -m1 -oE 'kVersion *= *"[0-9.]+"' src/app/version.hpp | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
name="render2d-$version-windows-x64"
dist="$root/dist"
out="$dist/$name"

step() { echo; echo "== $*"; }

# ---- 1. the Release --------------------------------------------------------------
step "1. build (make release)"
make release > /dev/null
exe="$root/build-release/render2d.exe"
[[ -x "$exe" ]] || { echo "no $exe after make release" >&2; exit 1; }
"$exe" --version | head -1

rm -rf "$out" "$dist/$name.zip"
mkdir -p "$out"
cp "$exe" "$out/"

# ---- 2. the DLLs ----------------------------------------------------------------
# ldd lists everything the executable loads, the DLLs of the DLLs included.
# Only those of MSYS2 are copied: Windows' own (kernel32, ...) are on every PC.
step "2. DLLs from $prefix/bin"
mapfile -t dlls < <(ldd "$exe" | grep -oE "$prefix/bin/[^ ]+\.dll" | sort -u)
for dll in "${dlls[@]}"; do
    cp "$dll" "$out/"
done
echo "${#dlls[@]} DLL(s)"

# ---- 3. GDAL and PROJ reference files ---------------------------------------------
step "3. share/gdal and share/proj"
mkdir -p "$out/share"
cp -r "$prefix/share/gdal" "$out/share/"
mkdir -p "$out/share/proj"
# proj.db and the small files; not the big transformation grids (*.tif),
# downloaded by PROJ on demand when one is ever needed.
find "$prefix/share/proj" -maxdepth 1 -type f ! -name '*.tif' -exec cp {} "$out/share/proj/" \;
[[ -f "$out/share/proj/proj.db" ]] || { echo "proj.db not found in $prefix/share/proj" >&2; exit 1; }

# ---- 4. styles, documents, licenses -----------------------------------------------
step "4. styles, README, licenses"
cp -r styles "$out/"
top="$(cd "$root/.." && pwd)"
[[ -f "$top/LICENSE" ]] && cp "$top/LICENSE" "$out/LICENSE.txt"

# The libraries' licenses: the MSYS2 package of each DLL, and its license
# folder. PACKAGES.txt lists the packages and their versions.
mkdir -p "$out/licenses"
: > "$out/licenses/PACKAGES.txt"
declare -A seen=()
for dll in "${dlls[@]}"; do
    pkg=$(pacman -Qqo "$dll" 2>/dev/null || true)
    [[ -z "$pkg" || -n "${seen[$pkg]:-}" ]] && continue
    seen[$pkg]=1
    pacman -Q "$pkg" >> "$out/licenses/PACKAGES.txt"
    if [[ -d "$prefix/share/licenses/${pkg#mingw-w64-ucrt-x86_64-}" ]]; then
        cp -r "$prefix/share/licenses/${pkg#mingw-w64-ucrt-x86_64-}" "$out/licenses/"
    fi
done
echo "$(wc -l < "$out/licenses/PACKAGES.txt") package(s), licenses in licenses/"

cat > "$out/README.txt" <<EOF
render2d $version for Windows (64-bit)
=====================================

A map renderer in the spirit of the GDAL tools: data and a style in, a map
out (PNG, SVG or PDF). https://github.com/nfr25/render2d
Design notes: https://nfr25.github.io/render2d/

Unzip anywhere, open a command prompt in this folder, and try:

    render2d --version
    render2d --help
    render2d parcels.shp --style styles\\showcase\\parcels.yaml -o map.png

Everything render2d needs is in this folder: the DLLs, and GDAL's and PROJ's
reference files in share\\. Nothing to install.

License: GNU AGPL v3 or later (LICENSE.txt). The libraries included keep
their own licenses (licenses\\, PACKAGES.txt lists them with their versions).
EOF
# Windows line endings for Notepad.
sed -i 's/$/\r/' "$out/README.txt"

# ---- 5. the check: without MSYS2 in the PATH ---------------------------------------
# Only Windows' own folders in the PATH: every DLL must come from our folder.
step "5. check, without MSYS2 in the PATH"
winpath="/c/Windows/System32:/c/Windows"
check=$(mktemp -d)
cat > "$check/square.geojson" <<'EOF'
{"type":"FeatureCollection",
 "crs":{"type":"name","properties":{"name":"urn:ogc:def:crs:EPSG::31370"}},
 "features":[{"type":"Feature","properties":{},
   "geometry":{"type":"Polygon","coordinates":[[[148000,170000],[149000,170000],[149000,171000],[148000,171000],[148000,170000]]]}}]}
EOF
(
    cd "$check"
    env -i PATH="$winpath" SYSTEMROOT="${SYSTEMROOT:-C:\\Windows}" "$out/render2d.exe" --version
    env -i PATH="$winpath" SYSTEMROOT="${SYSTEMROOT:-C:\\Windows}" "$out/render2d.exe" square.geojson --size 200 -o square.png > report.txt 2>&1
) || { echo "render2d does not start without MSYS2: a DLL is missing?" >&2; exit 1; }
cat "$check/report.txt"
grep -q 'EPSG:31370' "$check/report.txt" || { echo "the CRS was not recognised: is share/proj/proj.db found?" >&2; exit 1; }
[[ -s "$check/square.png" ]] || { echo "no image written" >&2; exit 1; }
rm -rf "$check"
echo "OK: starts and draws without MSYS2"

# ---- 6. the zip --------------------------------------------------------------------
step "6. zip"
cd "$dist"
if command -v zip > /dev/null; then
    zip -qr "$name.zip" "$name"
else
    powershell.exe -NoProfile -Command "Compress-Archive -Path '$name' -DestinationPath '$name.zip'"
fi
size=$(du -m "$name.zip" | cut -f1)
echo
echo "dist/$name.zip  ($size MB)"
echo "Next: GitHub > Releases > Draft a new release > tag v$version > drop the zip."
