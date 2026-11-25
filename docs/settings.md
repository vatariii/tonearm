# Settings reference

Every setting applies immediately and shows in the preview at the top of the properties window. Percentages are relative to the preset's own value, so 100% always means "as designed".

## Look

| Setting | What it does |
|---|---|
| **Preset** | Glass, Minimal, Neon, Editorial or Turntable. Changing the preset resets the visual settings below to that preset's values but keeps your width and height. |
| **Layout** | Compact, Expanded, Square or Vertical. Changing the layout also resets the size to the layout's recommended size. |
| **Width / Height** | The source's size in pixels. Type, spacing and radii scale with it, so a 1200 × 392 expanded card looks like a sharper 600 × 196 one. Horizontal layouts may grow wider without the text growing. |
| **Reset size for this layout** | Restores the recommended size for the current preset and layout. |
| **Show album artwork** | Hides the cover and lets the text use the space. |
| **Show album name** | The album line. Layouts drop it automatically when there isn't room, before anything else is squeezed. |
| **Show progress bar** | The bar and, depending on **Time**, the time labels. |
| **Time** | Elapsed and remaining, elapsed only, remaining only, or hidden. Compact layouts never show times. |
| **Show the player's name** | A small label such as APPLE MUSIC above the title. It is the first thing to go when space runs out. |

### Preset sizes

| Preset | Compact | Expanded | Square | Vertical |
|---|---|---|---|---|
| Glass | 460 × 100 | 600 × 196 | 360 × 360 | 300 × 460 |
| Minimal | 420 × 88 | 600 × 164 | 360 × 360 | 300 × 460 |
| Neon | 460 × 100 | 600 × 196 | 360 × 360 | 300 × 460 |
| Editorial | 480 × 108 | 600 × 212 | 360 × 360 | 300 × 500 |
| Turntable | 540 × 132 | 650 × 220 | 400 × 400 | 340 × 540 |

The source includes room for the drop shadow, so the visible card is slightly smaller than these numbers.

## Surface

| Setting | What it does |
|---|---|
| **Corner roundness** | 0% gives square corners, 200% doubles the preset's radius. |
| **Background opacity** | Transparency of the card. Text, artwork and the progress bar stay fully opaque. |
| **Shadow** | Strength of the card and artwork shadows. |
| **Artwork blur** | Glass and Editorial only. How much of the blurred artwork shows through the surface. |
| **Edge highlight** | The thin light line along the card's edge. In Neon it is the colored border. |
| **Glow** | Neon only. Strength of the glow around the card. |

## Color

| Setting | What it does |
|---|---|
| **Accent color** | *Match the album artwork* picks a vivid color from the cover for the progress bar and highlights. *Custom* uses the color you choose. |
| **Tint the background with the artwork** | When off, the card uses **Background color** instead of colors from the cover. |
| **Keep text readable** | Checks text against the pixels actually behind it and adjusts transparency or lightness when needed. Titles, artists and times are kept at 4.5:1 contrast (WCAG AA); the album line and accent at 3:1. Leave this on unless you have a specific reason. |
| **Use custom text colors** | Set your own title, artist and album colors, including transparency. **Keep text readable** still applies on top unless you switch it off. |

## Typography

| Setting | What it does |
|---|---|
| **Title typeface / Text typeface** | *Preset default* or one of the bundled families: Inter, Inter Display, IBM Plex Sans, IBM Plex Mono, EB Garamond. Titles in Chinese, Japanese, Korean and other scripts fall back to fonts installed with Windows. |
| **Text size** | 50% to 200% of the preset's sizes. Layouts re-flow; if text no longer fits it scrolls or is shortened. |

## Motion

| Setting | What it does |
|---|---|
| **Animation speed** | 25% to 300%. Higher is faster. |
| **Movement** | How far things travel during transitions. 0% leaves only fades. |
| **Reduce motion** | Replaces slides, scaling and scrolling with short crossfades, and shortens long titles with an ellipsis. Progress jumps straight to the new position after a seek. |
| **Scroll long titles** | When a title, artist or album doesn't fit, it scrolls slowly with soft edges, pausing at the start of each loop. When off, it is shortened with an ellipsis. |
| **Scroll speed** | In pixels per second at 1× size. |

## Playback states

| Setting | What it does |
|---|---|
| **When paused** | *Dim the overlay* lowers the artwork and text and fades the progress bar. *Keep it as it is* changes nothing. *Hide after a delay* fades the overlay out after **Hide after** seconds. |
| **When nothing is playing** | *Hide the overlay* fades it out when the player stops or closes. *Show a message* keeps the card up with your **Message**. |
| **Show sample tracks when nothing is playing** | Shows invented sample tracks so you can design the scene without music playing. They rotate every 12 seconds to show the transitions. Turn this off before going live. |

## Animated artwork (Experimental)

Off by default. The full description is in [Animated artwork](animated-artwork.md).

| Setting | Key | What it does |
|---|---|---|
| **Artwork** | `artwork_mode` | `static` (default) or `animated`: plays a matching video from the folder in place of the cover. |
| **Preferred orientation** | `motion_orientation` | `auto`, `square` or `portrait`. |
| **When music pauses** | `motion_pause` | `with_music` pauses the animation; `loop` keeps it looping. |
| **Animated artwork folder** | `motion_folder` | Folder of `Artist - Album.mp4` files (` [tall]` for portrait). |

These settings belong to the source, not to the look, so preset files don't include them. Reduce motion always shows the still cover.

## Music sources

| Setting | What it does |
|---|---|
| **Choose a player** | *Automatically*, *Prefer the apps below, in order* or *Only the apps below*. See [Getting started](getting-started.md#choosing-the-player). |
| **Apps** | App names, one per line, for example Apple Music, Spotify, TIDAL. Matching ignores case and spaces. |
| **Include web browsers** | Allows Chrome, Edge, Firefox, Opera, Brave, Vivaldi and Arc. Off by default. |
| **Detected now** | Read-only list of what Windows is reporting at the moment, with **Refresh**. |

## Preset files

| Setting | What it does |
|---|---|
| **Export to / Export preset** | Saves the visual, motion and playback-state settings to a JSON file. |
| **Import from / Import preset** | Loads such a file into this source. Music source settings are not part of preset files. |

The file format is:

```json
{
  "format": "tonearm-preset",
  "version": 1,
  "settings": { "preset": "glass", "layout": "expanded", "width": 600, "height": 196, "...": "..." }
}
```

## Controlling Tonearm from scripts

Settings use the keys shown in exported preset files, so they can be changed with obs-websocket's `SetInputSettings` or an OBS script. Changing `preset` or `layout` this way applies the preset exactly as the properties window would.
