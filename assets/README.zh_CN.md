<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

| 文件 | 规格 | 用途、来源与许可 |
| --- | --- | --- |
| [`fonts/NotoSansSC-Regular.ttf`](fonts/NotoSansSC-Regular.ttf) | TrueType 可变字重字体（默认 Regular 实例），约 17 MB | 番茄钟应用的中文界面字形。来源：[google/fonts `notosanssc`](https://github.com/google/fonts/tree/main/ofl/notosanssc)，SIL Open Font License 1.1（允许再分发）。 |

纳入版本管理的应用子集：`main/pomo_cjk_16.c` 与 `main/pomo_cjk_48.c`（后者仅含录音“开始”提示的两个字形）（16 px，4 bpp，未压缩，LVGL 格式；可打印 ASCII + 下方中文清单，约 15 KB `.rodata`）。

- 字符清单：可打印 ASCII（0x20-0x7E）+ 屏显用中文字符 81 个：`一中仅任休会作倒值停准分切到即听回声备复失好始完对将屏峰工已幅开式录恢意成持按换提新时暂束格次步没熄电省眠短结继续置蜂计设试话语说请调败轮返醒重量钟键长闪间静音鸣`（为应用全部中文串字面量的超集，含少量状态文案用字）
- 转换命令（固定版本 `lv_font_conv@1.5.3`，在仓库根目录执行）：

```bash
npx --yes lv_font_conv@1.5.3 \
  --font assets/fonts/NotoSansSC-Regular.ttf \
  --range 0x20-0x7E,0x4E00,0x4E2D,0x4EC5,0x4EFB,0x4F11,0x4F1A,0x4F5C,0x5012,0x503C,0x505C,0x51C6,0x5206,0x5207,0x5230,0x5373,0x542C,0x56DE,0x58F0,0x5907,0x590D,0x5931,0x597D,0x59CB,0x5B8C,0x5BF9,0x5C06,0x5C4F,0x5CF0,0x5DE5,0x5DF2,0x5E45,0x5F00,0x5F0F,0x5F55,0x6062,0x610F,0x6210,0x6301,0x6309,0x6362,0x63D0,0x65B0,0x65F6,0x6682,0x675F,0x683C,0x6B21,0x6B65,0x6CA1,0x7184,0x7535,0x7701,0x7720,0x77ED,0x7ED3,0x7EE7,0x7EED,0x7F6E,0x8702,0x8BA1,0x8BBE,0x8BD5,0x8BDD,0x8BED,0x8BF4,0x8BF7,0x8C03,0x8D25,0x8F6E,0x8FD4,0x9192,0x91CD,0x91CF,0x949F,0x952E,0x957F,0x95EA,0x95F4,0x9759,0x97F3,0x9E23 \



  --size 16 --bpp 4 --format lvgl --no-compress \
  --lv-font-name pomo_cjk_16 --lv-include lvgl.h \
  --output main/pomo_cjk_16.c
```

48px 录音提示子集（`main/pomo_cjk_48.c`）用同一命令生成，仅 `--symbols` 限定为提示文案、`--size 48`；屏显文案变化时一并重新生成。

- 屏显文案变化时用源码字面量重新提取清单并重生成，核对 Flash 占用（当前约 15 KB `.rodata`；ESP32-C3 无 PSRAM）。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。
