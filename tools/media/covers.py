#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turns covers saved by read-covers.ps1 into a track manifest for tonearm-render.

    python3 tools/media/covers.py COVERS_DIR OUTDIR

COVERS_DIR holds covers.json and the images it names. OUTDIR gets manifest.tsv
and one .rgba file per cover, which tonearm-render reads with --tracks. Covers
are album art from the user's own player and are not kept in the repository.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

from PIL import Image

ORDER = ["sos", "wonder", "merry-christmas", "positions", "143"]
# Approximate track lengths in seconds; the media session file has none.
DURATION = {"sos": 180, "wonder": 77, "merry-christmas": 272, "positions": 239, "143": 203}


def main() -> int:
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    entries = json.loads(Path(src / "covers.json").read_text(encoding="utf-8-sig"))
    by_slug = {Path(e["file"]).stem: e for e in entries}
    rows = []
    for slug in ORDER + sorted(set(by_slug) - set(ORDER)):
        e = by_slug.get(slug)
        if not e:
            continue
        im = Image.open(src / e["file"]).convert("RGBA")
        if max(im.size) > 1024:
            im = im.resize((1024, 1024), Image.LANCZOS)
        (out / f"{slug}.rgba").write_bytes(f"RGBA {im.width} {im.height}\n".encode() + im.tobytes())
        rows.append("\t".join([f"{slug}.rgba", e["title"], e["artist"], e["album"], str(DURATION.get(slug, 200))]))
    (out / "manifest.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    print(f"{len(rows)} tracks -> {out / 'manifest.tsv'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
