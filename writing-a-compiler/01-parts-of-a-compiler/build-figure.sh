#!/usr/bin/env bash
# Build pipeline.svg from pipeline.tex. Needs latex, dvisvgm and python3.
set -euo pipefail
cd "$(dirname "$0")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

latex -interaction=nonstopmode -halt-on-error -output-directory="$work" pipeline.tex >/dev/null
# The page size, not the drawn extent. The 6pt border that pipeline.tex
# gives the standalone page keeps the outer box strokes inside the view box.
dvisvgm --no-fonts --bbox=papersize "$work/pipeline.dvi" -o "$work/raw.svg" 2>/dev/null

python3 - "$work/raw.svg" pipeline.svg <<'PY'
import re
import sys

# The theme property behind each colour pipeline.tex uses. The page inlines
# the file with the site shortcode figsvg, so var(--ink) follows the site's
# light and dark buttons. The literal is the light value, kept as the
# fallback for the file opened on its own.
PROPERTIES = {
    "#0d1620": ("--ink", "#0D1620"),
    "#5a6874": ("--muted", "#5A6874"),
    "#00706b": ("--teal", "#00706B"),
}

def themed(match):
    tag, attrs, close = match.group(1), match.group(2), match.group(3)
    styles = []
    for prop in ("fill", "stroke"):
        found = re.search(r"\s%s='(#[0-9a-f]{6})'" % prop, attrs)
        if found and found.group(1) in PROPERTIES:
            name, literal = PROPERTIES[found.group(1)]
            styles.append("%s:var(%s, %s)" % (prop, name, literal))
            attrs = attrs.replace(found.group(0), "")
    if styles:
        attrs += " style='%s'" % ";".join(styles)
    return "<%s%s%s>" % (tag, attrs, close)

svg = open(sys.argv[1], encoding="utf-8").read()
svg = re.sub(r"<(\w+)((?:\s[^<>]*?)?)(/?)>", themed, svg)

# Prefix every id with the file name. An inlined drawing shares one id
# space with the page and every other figure on it.
svg = re.sub(r"(\sid=')([^']+)'", r"\1pipeline-\2'", svg)
svg = re.sub(r"(xlink:href='#)([^']+)'", r"\1pipeline-\2'", svg)

# Width and height in px with double quotes, the form an image render hook
# reads to reserve the space before the file loads.
def size(match):
    return '%s="%.2f"' % (match.group(1), float(match.group(2)) * 4 / 3)
root = re.search(r"<svg[^>]*>", svg).group(0)
svg = svg.replace(root, re.sub(r"\b(width|height)='([0-9.]+)pt'", size, root), 1)
open(sys.argv[2], "w", encoding="utf-8").write(svg)
PY
