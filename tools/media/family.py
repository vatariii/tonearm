# SPDX-License-Identifier: GPL-2.0-or-later
"""The four layouts of one preset arranged as a family on one stage.

    [vertical][square][ expanded      ]
    [        ][      ][ compact   ]

`layouts.webp` animates a real track change in all four at once (Editorial).
`presets/<preset>.webp` are the static sheets for the 20-combination comparison.
"""
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from common import *  # noqa: E402,F403

OUT = ROOT / "docs" / "media"
WORK = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/tonearm-media")
LAYOUTS = ["vertical", "square", "expanded", "compact"]


def geometry(preset, W, H, content_w, gap=48, rowgap=40):
    vw, vh = card_size(preset, "vertical")
    sw, sh = card_size(preset, "square")
    ew, eh = card_size(preset, "expanded")
    cw, ch = card_size(preset, "compact")
    # Solve at unit expanded scale, then fit to content width.
    se = 1.0
    sc = se
    col_h = eh * se + rowgap + ch * sc
    sv = col_h / vh
    ss = col_h / sh
    width = vw * sv + sw * ss + ew * se + 2 * gap
    k = (content_w - 2 * gap) / (width - 2 * gap)
    se, sc, sv, ss = se * k, sc * k, sv * k, ss * k
    rowgap_k = rowgap  # keep the vertical gap fixed
    col_h = eh * se + rowgap_k + ch * sc
    sv, ss = col_h / vh, col_h / sh
    width = vw * sv + sw * ss + ew * se + 2 * gap
    x = (W - width) / 2
    y = (H - col_h) / 2
    pos = {
        "vertical": (sv, x, y),
        "square": (ss, x + vw * sv + gap, y),
        "expanded": (se, x + vw * sv + gap + sw * ss + gap, y),
        "compact": (sc, x + vw * sv + gap + sw * ss + gap, y + eh * se + rowgap_k),
    }
    return pos


def still_sheet(preset, track, out, W=1760, H=620, content_w=1640, tint=None):
    pos = geometry(preset, W, H, content_w)
    art = render_still(Shot(preset, "square", track))
    bg = stage((W, H), tint=tint or dominant(art.crop(card_box(art))), glow_at=(0.15, -0.1))
    for layout in LAYOUTS:
        s, x, y = pos[layout]
        place_card(bg, render_still(Shot(preset, layout, track, scale=s)), x, y)
    save_webp(bg, out, quality=90)
    return bg


def animated(preset, tracks, out, W=1760, H=680, content_w=1600, hold=4.0, fps=30, tint=None):
    pos = geometry(preset, W, H, content_w)
    a, b = tracks
    start, c1 = 1.5, 1.5 + hold
    c2, end = c1 + hold, c1 + 2 * hold
    def run(layout):
        s = pos[layout][0]
        script = f"""
preset {preset}
layout {layout}
scale {s}
margin 40
fps {fps}
at 0 track {a} 60
at {c1} track {b} 2
at {c2} track {a} {60 + start - (end - c2):.4f}
record {start} {end} {{OUT}}
"""
        return layout, run_sequence(script, WORK / "family" / layout)
    with ThreadPoolExecutor(4) as ex:
        seqs = dict(ex.map(run, LAYOUTS))
    bg = stage((W, H), tint=tint, glow_at=(0.15, -0.1))
    layers = []
    for layout in LAYOUTS:
        frames = seqs[layout]
        l, t, r, b_ = card_box(Image.open(frames[0]).convert("RGBA"))
        _, x, y = pos[layout]
        layers.append((frames, (int(round(x)) - l, int(round(y)) - t)))
    comp = composite_frames(bg, layers, WORK / "family" / "comp")
    save_png(Image.open(comp[0]), WORK / "family" / "poster.png")
    encode_animation(comp, out_webp=out, fps=fps, webp_quality=82)


if __name__ == "__main__":
    which = sys.argv[2] if len(sys.argv) > 2 else "all"
    if which in ("all", "layouts"):
        animated("editorial", (1, 4), OUT / "layouts.webp", tint=(80, 170, 150))
    if which in ("all", "sheets"):
        for p in ["glass", "minimal", "neon", "editorial", "turntable"]:
            still_sheet(p, 2, OUT / "presets" / f"{p}.webp", tint=(170, 80, 200))
