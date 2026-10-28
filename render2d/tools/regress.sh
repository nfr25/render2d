#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Characterization test (notions §49): the same commands, before and after a
# change that must not change the result.
#
#   tools/regress.sh before [COMMANDS]   with the render2d you have now
#   (unzip the new sources, make release)
#   tools/regress.sh after  [COMMANDS]   with the new one, then compares
#
# COMMANDS: one render2d command per line, without the executable and without
# -o (default: tools/regress-commands.txt). {root} is replaced by the project
# folder. Results go to regress/before/ and regress/after/: NN.png and NN.txt
# (the report). PNGs must be identical byte for byte; reports are compared
# without the version line and without the milliseconds.
# The executable can be forced: R2D_EXE=/path/to/render2d tools/regress.sh ...

set -u
phase="${1:-}"
if [[ "$phase" != before && "$phase" != after ]]; then
    echo "usage: tools/regress.sh before|after [COMMANDS]" >&2
    exit 2
fi

root="$(cd "$(dirname "$0")/.." && pwd)"
commands="${2:-$root/tools/regress-commands.txt}"
# Windows form (C:/...) under MSYS2, so that render2d.exe understands it.
rootArg="$(cd "$root" && (pwd -W 2>/dev/null || pwd))"

exe="${R2D_EXE:-}"
if [[ -z "$exe" ]]; then
    for candidate in build-release/render2d.exe build-release/render2d build/render2d.exe build/render2d; do
        if [[ -x "$root/$candidate" ]]; then exe="$root/$candidate"; break; fi
    done
fi
if [[ -z "$exe" || ! -x "$exe" ]]; then
    echo "render2d executable not found (build it, or set R2D_EXE)" >&2
    exit 2
fi
echo "executable: $exe"
version="$("$exe" --version | head -1)"
echo "$version"

# "after" must use a NEW executable: the most common slip is to compare the
# old one with itself (built in build/ while build-release/ is still old).
if [[ "$phase" == after && -f "$root/regress/before/VERSION" ]] &&
   [[ "$(head -1 "$root/regress/before/VERSION")" == "${version%%,*}"* ]]; then
    echo "this is the executable of 'before' ($version): rebuild first (make release)," >&2
    echo "or point to the new one with R2D_EXE=..." >&2
    exit 2
fi

out="$root/regress/$phase"
rm -rf "$out"
mkdir -p "$out"
echo "${version%%,*}" > "$out/VERSION"     # "render2d 0.3.4 (step 3d: symbols)"

n=0
while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%$'\r'}"                               # files edited on Windows
    [[ -z "${line// }" || "$line" == \#* ]] && continue
    n=$((n + 1))
    name=$(printf '%02d' "$n")
    args="${line//\{root\}/$rootArg}"
    # Run inside the output folder: the report says "wrote 01.png" in both runs.
    (cd "$out" && eval "\"$exe\" $args -o $name.png" > "$name.txt" 2>&1; echo "exit code $?" >> "$name.txt")
    echo "  $name  $(tail -1 "$out/$name.txt")"
done < "$commands"
echo "$n command(s), results in regress/$phase/"

[[ "$phase" == before ]] && exit 0

# ---- compare --------------------------------------------------------------
before="$root/regress/before"
if [[ ! -d "$before" ]]; then
    echo "no regress/before/: run 'tools/regress.sh before' first, with the old executable" >&2
    exit 2
fi

# The version line and the timings change from run to run; nothing else may.
normalize() {
    sed -E -e '/^render2d [0-9]+\.[0-9]+\.[0-9]+/d' -e 's/ *[0-9]+\.[0-9]+ ms/ # ms/g' "$1"
}

differences=0
for report in "$before"/*.txt; do
    name="$(basename "$report" .txt)"
    status="same"
    if [[ ! -f "$out/$name.txt" ]]; then
        status="missing in after/"
    elif ! diff <(normalize "$report") <(normalize "$out/$name.txt") > "$out/$name.diff"; then
        status="REPORT DIFFERS (see regress/after/$name.diff)"
    elif [[ -f "$before/$name.png" ]] && ! cmp -s "$before/$name.png" "$out/$name.png"; then
        status="PNG DIFFERS"
    fi
    [[ -s "$out/$name.diff" ]] || rm -f "$out/$name.diff"
    [[ "$status" == same ]] || differences=$((differences + 1))
    echo "  $name  $status"
done

if (( differences == 0 )); then
    echo "OK: identical results"
    exit 0
fi
echo "$differences command(s) differ"
exit 1
