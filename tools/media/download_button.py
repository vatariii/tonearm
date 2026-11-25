# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds the README's "Download for Windows" button as outlined SVG.

Text is Inter SemiBold (bundled, SIL OFL 1.1) converted to paths, so the button
looks the same on GitHub without depending on installed fonts. Two versions:
a light pill for GitHub's dark theme and an ink pill for the light theme.
"""
from pathlib import Path

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs" / "media" / "brand"

font = TTFont(str(ROOT / "data" / "fonts" / "Inter-SemiBold.otf"))
gs, cmap, upm, hmtx = font.getGlyphSet(), font.getBestCmap(), font["head"].unitsPerEm, font["hmtx"]

TEXT = "Download for Windows"
SIZE = 17.0
H = 48.0
PAD_L, PAD_R, ICON, GAP = 22.0, 24.0, 16.0, 10.0
scale = SIZE / upm
cap = 0.727 * upm * scale     # Inter cap height

pen = SVGPathPen(gs)
x = 0.0
base = H / 2 + cap / 2
x0 = PAD_L + ICON + GAP
for ch in TEXT:
    g = cmap[ord(ch)]
    gs[g].draw(TransformPen(pen, (scale, 0, 0, -scale, x0 + x * scale, base)))
    x += hmtx[g][0]
text_path = pen.getCommands()
W = round(x0 + x * scale + PAD_R)


def svg(bg: str, fg: str, edge: str) -> str:
    # Download glyph: an arrow into a tray, drawn on the same 16 px box as the text's cap height.
    ix, iy = PAD_L, H / 2 - ICON / 2
    icon = (
        f'<path d="M{ix+8:.1f} {iy+1.5:.1f} V{iy+10:.1f} M{ix+4:.1f} {iy+6.5:.1f} L{ix+8:.1f} {iy+10.5:.1f} '
        f'L{ix+12:.1f} {iy+6.5:.1f} M{ix+2:.1f} {iy+11.5:.1f} V{iy+14.5:.1f} H{ix+14:.1f} V{iy+11.5:.1f}" '
        f'fill="none" stroke="{fg}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>'
    )
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{int(H)}" viewBox="0 0 {W} {int(H)}" '
        f'role="img" aria-labelledby="t"><title id="t">Download Tonearm for Windows</title>'
        f'<rect x=".5" y=".5" width="{W-1}" height="{H-1:.0f}" rx="{(H-1)/2:.1f}" fill="{bg}" stroke="{edge}"/>'
        f'{icon}<path d="{text_path}" fill="{fg}"/></svg>\n'
    )


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    # Dark theme: a paper pill. Light theme: an ink pill. Colors from docs/media/DESIGN.md.
    (OUT / "download-dark.svg").write_text(svg("#F2EEE8", "#18161D", "#F2EEE8"))
    (OUT / "download-light.svg").write_text(svg("#1F1D24", "#F2EEE8", "#1F1D24"))
    print(W, "x", int(H))
