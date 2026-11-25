# SPDX-License-Identifier: GPL-2.0-or-later
"""presets.webp: one continuous Tonearm run that cycles the five presets.
Switches use the plugin's own style transition; the caption row marks which
preset is on screen."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from common import *  # noqa: E402,F403

OUT = ROOT / "docs" / "media"
WORK = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/tonearm-media")


def caption_row(size, labels, active, alpha_active=1.0):
    """Row of labels; the active one in paper, the rest muted. Returns RGBA."""
    w, h = size
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    f = font("Inter-Medium.otf", 30)
    gap = 64
    widths = [d.textlength(s, font=f) for s in labels]
    x = (w - (sum(widths) + gap * (len(labels) - 1))) / 2
    for i, (s, tw) in enumerate(zip(labels, widths)):
        a = active.get(i, 0.0)
        col = tuple(int(MUTED[k] + (PAPER[k] - MUTED[k]) * a) for k in range(3))
        op = int(255 * (0.55 + 0.45 * a))
        d.text((x, h / 2), s, font=f, fill=col + (op,), anchor="lm")
        # Short rule under the active label.
        if a > 0.01:
            d.rounded_rectangle((x, h / 2 + 26, x + tw, h / 2 + 29), 2, fill=PAPER + (int(255 * a),))
        x += tw + gap
    return img


def build(name, labels, styles, scale, track, size, tint, hold=3.0, fps=30, card_y=None):
    W, H = size
    t0 = 3.8
    # The window opens 0.2 s before the first switch and closes on the first style again.
    switches = [t0 + 0.2 + hold * i for i in range(len(styles))]
    end = t0 + hold * len(styles)
    lines = [f"preset {styles[0][0]}", f"layout {styles[0][1]}", f"scale {scale}", "margin 40", f"fps {fps}",
             f"at 0 track {track} 40"]
    for i, ts in enumerate(switches):
        p, l = styles[(i + 1) % len(styles)]
        lines.append(f"at {ts:.3f} style {p} {l}")
    # Return to the first style with a seek, so the loop closes on the frame it opened on.
    lines.append(f"at {switches[-1]:.3f} track {track} {40 + t0 - (end - switches[-1]):.4f}")
    lines.append(f"record {t0} {end} {{OUT}}")
    frames = run_sequence("\n".join(lines) + "\n", WORK / name / "seq")
    first = Image.open(frames[0]).convert("RGBA")
    fw, fh = first.size
    caption_h = 110
    cy = card_y if card_y is not None else (H - caption_h - fh) / 2 + 10
    offset = (int((W - fw) / 2), int(cy))
    bg = stage((W, H), tint=tint, glow_at=(0.5, -0.1))
    outdir = WORK / name / "comp"
    outdir.mkdir(parents=True, exist_ok=True)
    comp = []
    for i, fp in enumerate(frames):
        t = t0 + i / fps
        # Which label is active, with a short crossfade matching the plugin's transition.
        idx, a = 0, 1.0
        k = sum(1 for s in switches if t >= s)
        idx = k % len(styles)
        since = t - switches[k - 1] if k > 0 else 99
        active = {idx: 1.0}
        if since < 0.25:
            a = since / 0.25
            active = {idx: a, (idx - 1) % len(styles): 1 - a}
        img = bg.copy()
        img.alpha_composite(Image.open(fp).convert("RGBA"), offset)
        img.alpha_composite(caption_row((W, caption_h), labels, active), (0, H - caption_h - 36))
        img.putalpha(bg.getchannel("A"))
        p = outdir / f"c{i:05d}.png"
        img.save(p, compress_level=1)
        comp.append(p)
    save_png(Image.open(comp[0]), WORK / name / "poster.png")
    encode_animation(comp, out_webp=OUT / f"{name}.webp", fps=fps, webp_quality=82)
    return comp


if __name__ == "__main__":
    which = sys.argv[2] if len(sys.argv) > 2 else "both"
    if which in ("both", "presets"):
        build("presets", ["Glass", "Minimal", "Neon", "Editorial", "Turntable"],
              [("glass", "expanded"), ("minimal", "expanded"), ("neon", "expanded"), ("editorial", "expanded"),
               ("turntable", "expanded")],
              scale=2.1, track=1, size=(1760, 660), tint=(76, 178, 150))
