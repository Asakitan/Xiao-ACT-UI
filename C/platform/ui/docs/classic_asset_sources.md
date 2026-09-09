# Classic SAO UI asset sources

This directory contains a small, dependency-free monochrome SVG set for the classic SAO-style interface. The icons are original geometric redraws created for this project, not copied from the SAO Utils depot or from a fan reupload. Each file uses a transparent `24x24` viewBox, `currentColor`, and simple SVG path data so the same files can be consumed by native path renderers and web CSS.

## Included assets

`menu`, `confirm`, `cancel`, `settings`, `tools`, `plugins`, `user`, `notification`, `back`, `home`, `folder`, `search`, `lock`, `info`, `download`, `expand`, `refresh`, `close`, `chat`, `keyboard`, `workshop`, and `process`.

Use the `manifest.json` file beside the SVGs as the source-of-truth inventory. It records the generated source, license, and SHA-256 for every asset.

## License and provenance

- The original SVG redraws in this folder are released as **CC0-1.0** for project reuse. The color is intentionally left to the renderer (`currentColor`); recommended SAO palette values are cool gray and amber/orange accents.
- The checked reference icon project is [vm-sao/sao-icons](https://github.com/vm-sao/sao-icons), whose README and `package.json` identify MIT licensing. Its checked `main` history includes commit `fd677ca` (2025-02-24). It was used as a category/reference index only; these files are original redraws.
- The SAOUI display font is now copied byte-for-byte from `python/assets/fonts/SAOUI.ttf` to `C/platform/ui/assets/fonts/SAOUI.ttf` and embedded by the native DirectWrite resource path. The font metadata has no license field. The upstream fan-font author, darkblackswords, describes SAO UI / SAO Welcome as fan-made, personal-use, non-commercial material on [the original DeviantArt page](https://www.deviantart.com/darkblackswords/art/Sword-Art-Online-Font-Download-426603647); this remains provenance only and is not a redistribution grant. `C/platform/ui/assets/fonts/SOURCE.md` records the SHA-256 and distribution review status.
- Original/repository sound provenance is tracked separately in `C/platform/ui/assets/sounds/manifest.json`. The 11 existing WAVs have not received a new license verification; the four notification cues are original synthesized outputs from `generate_notification_cues.py`. No sound is labeled CC0. The six user-guide sound copies under `C/launcher/docs/html/assets/sounds/` are recorded with matching SHA-256 values.
- `ZhuZi` is a F方正-restricted font and is deliberately not copied into this folder.

## Suggested consumption

1. Web: import the SVG as an `<img>` or inline it and set `color`/`stroke: currentColor` on the host.
2. Native: parse each path from the SVG and apply a renderer tint at draw time; no raster asset or font dependency is required.
3. State variants: retain one geometry and switch tint/opacity for normal, hover, pressed, selected, and disabled states. This mirrors the state-oriented structure observed in the SAO Utils theme package while keeping the new assets independently reusable.

## Native path consumer

`C/platform/ui/assets/classic/classic_icons.h` contains the same line geometry as the SVG set, including the four semantic additions `Chat`, `Keyboard`, `Workshop`, and `Process`. Its `paint_classic_icon(ctx, id, x, y, size, argb)` helper emits the strokes through the existing `sao_ui_paint_ctx_stroke_line` ABI. `entity_shell.cpp` uses the shared table for its BGRA raster path, mapping built-in root/child action IDs to vector symbols while leaving non-empty plugin-provided icon strings on the text path.
