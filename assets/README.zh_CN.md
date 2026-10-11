<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### `fonts/game2048_font_16.c`

| 项目 | 内容 |
| --- | --- |
| 用途 | `main/` 中 2048 应用的中文界面文本，名义字级 16 px。 |
| 来源字体 | [Noto Sans CJK SC Regular](https://github.com/notofonts/noto-cjk)（`Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`），SIL Open Font License 1.1，允许再分发。 |
| 字符范围 | ASCII `0x20`–`0x7E`，加上 [`fonts/game2048_symbols.txt`](fonts/game2048_symbols.txt) 列出的 50 个汉字。 |
| 转换命令 | `lv_font_conv --font NotoSansCJKsc-Regular.otf -r 0x20-0x7E --symbols "$(cat assets/fonts/game2048_symbols.txt)" --size 16 --bpp 4 --format lvgl --no-compress --lv-font-name game2048_font_16 --lv-include lvgl.h -o assets/fonts/game2048_font_16.c` |
| 集成方式 | 由 `main/CMakeLists.txt` 通过 `target_sources()` 编译；在 `main/g2048_ui.c` 里用 `LV_FONT_DECLARE(game2048_font_16)` 声明并显式绑定到控件。 |
| 开销 | 145 个字形，4 bpp 未压缩；位图约 9.5 KB，存放在只读 Flash。 |
| 度量 | `line_height` 为 20 px、`base_line` 为 5 px —— 排版按每行 20 px 预留，不是 16 px。 |

为什么要自建子集：Montserrat 不含汉字，而 LVGL 内置的
`lv_font_source_han_sans_sc_16_cjk` 覆盖的是一批与本界面完全无关的生僻字。
给界面加中文是字库工程，不是把字符串改成 UTF-8 就够了。

覆盖情况由 `tools/check_app_font_coverage.py` 把关，它属于静态门禁：解析生成的
cmap 表、核对字符清单与 `main/g2048_ui.c` 里的每一个字符串字面量，并检查字形度量
是否合理。界面文案一变，就要重新生成字库并重跑这个检查。

### `fonts/whale_ui_font_16.c` / `fonts/whale_ui_font_24.c`

| 项目 | 内容 |
| --- | --- |
| 用途 | `main/whale_ui.c`（小鲸鱼计费名片）的中文界面文本：16 px 用于正文，24 px 用于泡泡卡主数值。 |
| 来源字体 | [Noto Sans CJK SC Regular](https://github.com/notofonts/noto-cjk)（`Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`），SIL Open Font License 1.1，允许再分发。 |
| 字符范围 | ASCII `0x20`–`0x7E`，加上 [`fonts/whale_ui_font_symbols.txt`](fonts/whale_ui_font_symbols.txt) 列出的 139 个非 ASCII 码位（含 `¥`、`·`、`…`）。 |
| 转换命令 | `python3 tools/gen_whale_font.py`（内部按两个字号分别调用 `lv_font_conv`，字表由扫描 `main/whale_ui.c` 的字符串字面量得到）。 |
| 集成方式 | 由 `main/CMakeLists.txt` 通过 `target_sources()` 编译；在 `main/whale_ui.c` 里用 `LV_FONT_DECLARE()` 声明并显式绑定到控件。 |
| 开销 | 每级 234 个字形，4 bpp 未压缩；位图存放在只读 Flash（16 px 约 14 KB，24 px 约 28 KB）。 |
| 度量 | 16 px：`line_height` 20、`base_line` 5；24 px：`line_height` 29、`base_line` 7。排版按这些值预留行高。 |

覆盖情况由 `tools/check_whale_font_coverage.py` 把关（同样是静态门禁）：**两个字号**
都要能画出字表与界面文案里的每个字，并检查汉字字形尺寸是否合理。

动态文本（SSID、IP、时间）不进字表，一律经 `whale_text_ascii_preview()` 压成 ASCII
预览后再显示：设备字库是按固定文案做的子集，盖不住任意汉字。

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
