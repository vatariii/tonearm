# SPDX-License-Identifier: GPL-2.0-or-later
"""README hero: four presets on one stage; the Turntable card plays real track changes."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from common import *  # noqa: E402,F403

OUT = ROOT / "docs" / "media"
WORK = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/tonearm-media") / "hero"

W, H = 1760, 800
gwi, ghi = card_size("turntable", "expanded")
nwi, nhi = card_size("neon", "compact")
mwi, mhi = card_size("glass", "compact")
vwi, vhi = card_size("editorial", "vertical")
sg, gap, gap2, colgap = 2.08, 52, 48, 60
sc = (gwi * sg - gap) / (nwi + mwi)
sv = (ghi * sg + gap2 + nhi * sc) / vhi
gw, gh, vw, vh = gwi * sg, ghi * sg, vwi * sv, vhi * sv
x0 = (W - (gw + colgap + vw)) / 2
y0 = (H - vh) / 2

bg = stage((W, H), tint=(226, 128, 84), glow_at=(0.22, -0.05))
yb = y0 + gh + gap2
place_card(bg, render_still(Shot("neon", "compact", 2, scale=sc)), x0, yb)
place_card(bg, render_still(Shot("glass", "compact", 4, scale=sc)), x0 + nwi * sc + gap, yb + (nhi - mhi) * sc / 2)
place_card(bg, render_still(Shot("editorial", "vertical", 1, scale=sv)), x0 + gw + colgap, y0)

# Track 0, then the long-titled track 3 (its title scrolls), then back to track 0.
# The return lands on the same position the loop starts on, so the loop is seamless.
start, change1, change2, end = 1.5, 5.0, 11.4, 15.0
script = f"""
preset turntable
layout expanded
scale {sg}
margin 40
fps 30
at 0 track 0 {60.0}
at {change1} track 3 2
at {change2} track 0 {60.0 + start - (end - change2):.4f}
record {start} {end} {{OUT}}
"""
frames = run_sequence(script, WORK / "turntable")
l, t, r, b = card_box(Image.open(frames[0]).convert("RGBA"))
offset = (int(round(x0)) - l, int(round(y0)) - t)
comp = composite_frames(bg, [(frames, offset)], WORK / "comp")
save_png(Image.open(comp[0]), WORK / "poster.png")
encode_animation(comp, out_webp=OUT / "hero.webp", out_mp4=None, fps=30, webp_quality=82)
print("hero frames:", len(comp))
