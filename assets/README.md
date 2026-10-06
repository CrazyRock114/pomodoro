<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

| File | Details | Use, source, and license |
| --- | --- | --- |
| [`fonts/NotoSansSC-Regular.ttf`](fonts/NotoSansSC-Regular.ttf) | TrueType variable font (default Regular instance), ~17 MB | Chinese UI glyphs for the pomodoro application. Source: [google/fonts `notosanssc`](https://github.com/google/fonts/tree/main/ofl/notosanssc), SIL Open Font License 1.1 (redistribution permitted). |

Generated application subsets kept under version control: `main/pomo_cjk_16.c` and `main/pomo_cjk_48.c` (the latter holds only the two glyphs of the recording "start" cue) (16 px, 4 bpp, uncompressed, LVGL format; printable ASCII plus the CJK inventory below, ~15 KB of `.rodata`).

- Character inventory: printable ASCII (0x20-0x7E) plus 81 CJK code points used by on-screen text; the literal list is in the paired Chinese document. The subset is a superset of every Chinese string literal rendered by the application.
- Conversion command (pinned `lv_font_conv@1.5.3`, run from the repository root):

```bash
npx --yes lv_font_conv@1.5.3 \
  --font assets/fonts/NotoSansSC-Regular.ttf \
  --range 0x20-0x7E,0x4E00,0x4E2D,0x4EC5,0x4EFB,0x4F11,0x4F1A,0x4F5C,0x5012,0x503C,0x505C,0x51C6,0x5206,0x5207,0x5230,0x5373,0x542C,0x56DE,0x58F0,0x5907,0x590D,0x5931,0x597D,0x59CB,0x5B8C,0x5BF9,0x5C06,0x5C4F,0x5CF0,0x5DE5,0x5DF2,0x5E45,0x5F00,0x5F0F,0x5F55,0x6062,0x610F,0x6210,0x6301,0x6309,0x6362,0x63D0,0x65B0,0x65F6,0x6682,0x675F,0x683C,0x6B21,0x6B65,0x6CA1,0x7184,0x7535,0x7701,0x7720,0x77ED,0x7ED3,0x7EE7,0x7EED,0x7F6E,0x8702,0x8BA1,0x8BBE,0x8BD5,0x8BDD,0x8BED,0x8BF4,0x8BF7,0x8C03,0x8D25,0x8F6E,0x8FD4,0x9192,0x91CD,0x91CF,0x949F,0x952E,0x957F,0x95EA,0x95F4,0x9759,0x97F3,0x9E23 \



  --size 16 --bpp 4 --format lvgl --no-compress \
  --lv-font-name pomo_cjk_16 --lv-include lvgl.h \
  --output main/pomo_cjk_16.c
```

The 48 px recording cue subset (`main/pomo_cjk_48.c`) is generated the same way with `--symbols` limited to the cue text and `--size 48`; regenerate both when on-screen text changes.

- When the inventory changes, regenerate the file and re-check Flash impact (the subset adds ~19 KB of `.rodata`; the ESP32-C3 has no PSRAM).

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

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
