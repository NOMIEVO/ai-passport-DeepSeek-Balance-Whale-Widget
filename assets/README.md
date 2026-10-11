<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### `fonts/game2048_font_16.c`

| Item | Value |
| --- | --- |
| Purpose | Chinese UI text for the 2048 application in `main/`, at a nominal 16 px size. |
| Source font | [Noto Sans CJK SC Regular](https://github.com/notofonts/noto-cjk) (`Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`), SIL Open Font License 1.1 — redistribution permitted. |
| Character range | ASCII `0x20`–`0x7E` plus the 50 CJK characters listed in [`fonts/game2048_symbols.txt`](fonts/game2048_symbols.txt). |
| Conversion | `lv_font_conv --font NotoSansCJKsc-Regular.otf -r 0x20-0x7E --symbols "$(cat assets/fonts/game2048_symbols.txt)" --size 16 --bpp 4 --format lvgl --no-compress --lv-font-name game2048_font_16 --lv-include lvgl.h -o assets/fonts/game2048_font_16.c` |
| Integration | Compiled by `main/CMakeLists.txt` through `target_sources()`; declared with `LV_FONT_DECLARE(game2048_font_16)` and selected explicitly in `main/g2048_ui.c`. |
| Cost | 145 glyphs, 4 bpp uncompressed; the bitmap table is about 9.5 KB and lives in read-only Flash. |
| Metrics | `line_height` is 20 px and `base_line` is 5 px — reserve 20 px per text line, not 16. |

Why a purpose-built subset: Montserrat has no CJK glyphs, and LVGL's built-in
`lv_font_source_han_sans_sc_16_cjk` covers an unrelated set of rare characters —
none of the characters this UI actually prints. Adding Chinese text is a font
task, not a translation task.

Coverage is enforced by `tools/check_app_font_coverage.py`, which is part of the
static gate: it resolves the generated cmap tables, checks the tracked inventory
and every string literal in `main/g2048_ui.c`, and rejects implausible glyph
metrics. Regenerate the font and rerun that check whenever the UI text changes.

### `fonts/whale_ui_font_16.c` / `fonts/whale_ui_font_24.c`

| Item | Value |
| --- | --- |
| Purpose | Chinese UI text for the whale billing card in `main/whale_ui.c`: 16 px body text, 24 px bubble-card main value. |
| Source font | [Noto Sans CJK SC Regular](https://github.com/notofonts/noto-cjk) (`Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`), SIL Open Font License 1.1 — redistribution permitted. |
| Character range | ASCII `0x20`–`0x7E` plus the 139 non-ASCII code points listed in [`fonts/whale_ui_font_symbols.txt`](fonts/whale_ui_font_symbols.txt) (including `¥`, `·`, `…`). |
| Conversion | `python3 tools/gen_whale_font.py` — it scans the string literals of `main/whale_ui.c` for the inventory and calls `lv_font_conv` once per size. |
| Integration | Compiled by `main/CMakeLists.txt` through `target_sources()`; declared with `LV_FONT_DECLARE()` and selected explicitly in `main/whale_ui.c`. |
| Cost | 234 glyphs per size, 4 bpp uncompressed; the bitmap tables are about 14 KB (16 px) and 28 KB (24 px) in read-only Flash. |
| Metrics | 16 px: `line_height` 20, `base_line` 5. 24 px: `line_height` 29, `base_line` 7. Reserve those line heights when laying out text. |

Coverage is enforced by `tools/check_whale_font_coverage.py`, also part of the
static gate: **both sizes** must cover the inventory and every UI string, with
plausible CJK glyph metrics.

Dynamic text (SSID, IP, time) is not part of the inventory. It is rendered through
`whale_text_ascii_preview()`, which keeps ASCII and replaces every other byte with
`?`: the device font is a subset of the fixed UI copy and cannot cover arbitrary
Han characters.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
