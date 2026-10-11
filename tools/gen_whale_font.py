#!/usr/bin/env python3
"""生成小鲸鱼计费名片界面用的中文字体子集(16 px / 24 px)。

LVGL 内置的 lv_font_source_han_sans_sc_16_cjk 只有 1433 个生僻码位,不含本界面
真正要用的「余 额 峰 谷 刷 新 设 置 …」等字,会渲染成空白;Montserrat 更是没有
CJK。所以这里按界面实际文案生成一份可控子集。

字表不手写:直接扫描 main/whale_ui.c 的字符串字面量,把非 ASCII 字符收集起来,
避免"界面加了字却忘了重生成字体"这类漏字。

用法:
    python3 tools/gen_whale_font.py
    python3 tools/gen_whale_font.py --font /path/to/NotoSansCJKsc-Regular.otf

产物:
    assets/fonts/whale_ui_font_16.c               16 px 字体(界面正文)
    assets/fonts/whale_ui_font_24.c               24 px 字体(泡泡卡主数值)
    assets/fonts/whale_ui_symbols.txt             本次用到的字表(纳入版本管理)
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
FONT_NAME = "whale_ui_font"
SIZES = (16, 24)
OUT_DIR = REPO_ROOT / "assets" / "fonts"
OUT_INVENTORY = OUT_DIR / f"{FONT_NAME}_symbols.txt"

# 扫描范围:界面文案主要写在 whale_ui.c,但峰谷样式这类"要被主机测试断言"的文案常量
# 放在 whale_model.c —— 两边一起扫,tools/check_whale_font_coverage.py 必须保持一致。
SCAN_SOURCES = ["main/whale_ui.c", "main/whale_model.c"]

# 默认字体源:Noto Sans CJK SC(SIL OFL 1.1,允许随固件再分发)。它不在仓库里,
# 换机器时用 --font 指定同授权的其它字体。
DEFAULT_FONT_CANDIDATES = [
    Path("E:/Workbuddy/MPD/font-src/NotoSansCJKsc-Regular.otf"),
    Path("C:/Windows/Fonts/NotoSansSC-Regular.otf"),
    Path("C:/Windows/Fonts/NotoSansSC-VF.ttf"),
    Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
]

DEFAULT_CONV_CANDIDATES = [
    Path.home() / ".workbuddy/binaries/node/workspace/node_modules/lv_font_conv/lv_font_conv.js",
    Path("lv_font_conv"),
]

# 只取单行内的字符串字面量。跨行匹配会把注释里的中文一起卷进来,
# 导致字表虚胖(字体变大)且难以审阅。tools/check_whale_font_coverage.py
# 必须用同一条规则,否则两边判定会不一致。
STRING_LITERAL_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def collect_symbols(sources: list[str]) -> list[str]:
    """扫描源码里的字符串字面量,收集所有非 ASCII 字符。"""
    found: set[str] = set()
    for rel in sources:
        path = REPO_ROOT / rel
        if not path.is_file():
            print(f"跳过不存在的源文件: {rel}", file=sys.stderr)
            continue
        text = path.read_text(encoding="utf-8")
        for literal in STRING_LITERAL_RE.findall(text):
            try:
                # 只处理 \n \t 这类简单转义;中文不会走这条路径。
                decoded = literal.encode("utf-8").decode("unicode_escape").encode(
                    "latin-1", "ignore").decode("utf-8", "ignore")
            except UnicodeDecodeError:
                decoded = literal
            for ch in decoded:
                if ord(ch) > 0x7F:
                    found.add(ch)
    return sorted(found, key=lambda c: ord(c))


def resolve_font(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise SystemExit(f"指定的字体源不存在: {path}")
        return path
    for candidate in DEFAULT_FONT_CANDIDATES:
        if candidate.is_file():
            return candidate
    raise SystemExit("找不到默认字体源,请用 --font 指定 Noto Sans CJK/SC 字体文件")


NODE_CANDIDATES = [
    Path.home() / ".workbuddy/binaries/node/versions/22.22.2-6/node.exe",
    Path("node"),
]


def resolve_node() -> str:
    for candidate in NODE_CANDIDATES:
        if candidate.is_file():
            return str(candidate)
        if candidate.name == "node":
            return "node"
    raise SystemExit("找不到 node,无法运行 lv_font_conv")


def resolve_converter() -> list[str]:
    for candidate in DEFAULT_CONV_CANDIDATES:
        if candidate.is_file():
            # lv_font_conv 是 JS 写的,本机路径下要用 node 显式启动。
            if candidate.suffix == ".js":
                return [resolve_node(), str(candidate)]
            return [str(candidate)]
        if candidate.name == "lv_font_conv":
            return ["lv_font_conv"]
    raise SystemExit("找不到 lv_font_conv,请先安装:npm i -g lv_font_conv")


def main() -> int:
    parser = argparse.ArgumentParser(description="生成小鲸鱼计费名片的界面字体子集")
    parser.add_argument("--font", help="源字体文件(OTF/TTF)")
    args = parser.parse_args()

    font_path = resolve_font(args.font)
    converter = resolve_converter()
    symbols = collect_symbols(SCAN_SOURCES)
    if not symbols:
        raise SystemExit("没有扫描到任何非 ASCII 字符,检查 SCAN_SOURCES 配置")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    OUT_INVENTORY.write_text("".join(symbols) + "\n", encoding="utf-8")

    for size in SIZES:
        out_c = OUT_DIR / f"{FONT_NAME}_{size}.c"
        cmd = [
            *converter,
            "--font", str(font_path),
            "-r", "0x20-0x7E",
            "--symbols", "".join(symbols),
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            "--no-compress",
            "--lv-font-name", f"{FONT_NAME}_{size}",
            "--lv-include", "lvgl.h",
            "-o", str(out_c),
        ]
        print("运行:", " ".join(cmd))
        subprocess.run(cmd, check=True)

    codepoints = ", ".join(f"U+{ord(c):04X}" for c in symbols)
    print(f"字表({len(symbols)} 个非 ASCII 码位):{codepoints}")
    print(f"字表文件:{OUT_INVENTORY}")
    for size in SIZES:
        print(f"生成字体:{OUT_DIR / f'{FONT_NAME}_{size}.c'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
