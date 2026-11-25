# Media design

The rules behind the images in the README and docs, so new media stays consistent. Every script mentioned here lives in [`tools/media`](../../tools/media).

## Idea

Tonearm's cards are the subject, so the page around them stays dark, quiet and neutral, like a stage with the lights down. Colour comes from the record that's playing: every stage takes a faint tint from the artwork it shows. Outside the Turntable preset itself, the only record-player references are a set of wide, almost invisible rings in the corner of each stage and the small mark next to the wordmark.

## Tokens

| Token | Value | Use |
|---|---|---|
| Groove | `#18161D` (24, 22, 29) | Stage base, MP4 background |
| Lacquer | `#24212B` (36, 33, 43) | Top of the stage gradient |
| Paper | `#F2EEE8` (242, 238, 232) | Captions on a stage |
| Muted | `#A49EAC` (164, 158, 172) | Secondary captions |
| Ink (light theme) | `#1F1D24` | Wordmark on light backgrounds |
| Ink (dark theme) | `#ECE8F0` | Wordmark on dark backgrounds |
| Badge | `#4A4458` | Shields.io badge colour |

Stage tints used so far: hero `(226, 128, 84)` (Turntable at the center since 0.3.0), presets `(76, 178, 150)`, layouts `(80, 170, 150)`, preset sheets `(170, 80, 200)`, animated artwork collection `(120, 132, 170)`. Per-album clips take the median colour of their own cover.

## Type

- **Wordmark:** EB Garamond 12 Regular, lowercase "tonearm", tracking −0.012 em, outlined to SVG paths by `wordmark.py` so it never depends on installed fonts. The mark is a record (34 px radius, 3 px stroke, one groove ring at 45 % opacity) with an arm resting on it.
- **Captions inside images:** Inter Medium and SemiBold, Paper and Muted on the stage.
- Everything inside a card is Tonearm's own typography for that preset; media never restyles it.

## Dimensions

- Full-width images are **1760 px** wide (about 2× GitHub's 880 px README column) and shown with `width="880"`.
- Stage corner radius 24 px at 2× (12 px displayed), baked into the image with transparency, because GitHub doesn't round images.
- Card corner radius in OBS recordings measures 33 px at the recorded size; `artwork.py` masks with that value.
- Spacing on stages is in multiples of 8 px at 2×; card gaps are 40–60 px.
- MP4 dimensions are always even.

## Motion

- 30 fps, seamless loops. Rendered loops are closed by seeking the timeline back to its start; recorded loops are closed with a 0.6 s crossfade.
- One thing moves at a time where possible: the hero changes one card's track, the preset loop changes one style every 3 s across all five presets, the layouts loop changes track every 4 s.
- No invented motion. Rendered media comes from Tonearm's real scene, rasterizer and compositor (`tonearm-render`, `tonearm-sequence`). Animated artwork comes only from recordings of the plugin in OBS.

## Formats

| Format | Where | Settings |
|---|---|---|
| Animated WebP | Shown inline in the README and docs | `libwebp_anim`, yuva420p, quality 65–82 |
| GIF | Fallback link | 20 fps, 200-colour palette, ordered dither |
| MP4 | Linked for full quality | H.264, CRF 18, yuv420p, BT.709 tags, faststart, no audio |
| SVG | Wordmark | Outlined paths, `role="img"` with a title |

Keep a README image under about 6 MB. Every image gets alt text that says what is happening, not just what it is.

## Regenerating

```sh
cmake -S . -B build -DTONEARM_BUILD_PLUGIN=OFF && cmake --build build   # tonearm-render
tools/media/build-sequence.sh                                            # tonearm-sequence
python3 tools/media/wordmark.py
python3 tools/media/hero.py
python3 tools/media/cycles.py
python3 tools/media/family.py
python3 tools/media/artwork.py RECORDING.mp4 docs/media/artwork
```

`artwork.py` needs an OBS recording made with the four-source scene described at the top of the script. The recording itself is not kept in the repository.
