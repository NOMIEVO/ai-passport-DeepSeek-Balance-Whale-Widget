#!/usr/bin/env python3
"""Static glyph-coverage gate for the whale billing card's Chinese UI.

A UTF-8 source file and a successful build do not prove that LVGL can draw the
text: Montserrat has no CJK glyphs and LVGL's built-in Source Han Sans SC
"CJK" subset covers an unrelated set of rare characters, none of which this UI
uses. This script closes that gap on the host for **both** generated sizes:

1. every code point in the tracked character inventory exists in each font's
   cmap tables (offsets are resolved the way LVGL's FORMAT0/SPARSE tables are
   actually decoded, so a wrong assumption fails here, not on screen);
2. every non-ASCII code point that appears in a real string literal of the UI
   sources is covered (comments are stripped first, so explanatory Chinese
   comments are not mistaken for UI text);
3. the generated CJK glyphs have plausible metrics, which catches the corrupt
   bitmap output a converter can emit for a subset like this.

Run: python3 tools/check_whale_font_coverage.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "assets" / "fonts"
FONT_NAME = "whale_ui_font"
SIZES = (16, 24)
INVENTORY = FONT_DIR / f"{FONT_NAME}_symbols.txt"
# 界面文案写在 UI 层与模型层:模型层放了峰谷样式等要被测试断言的文案常量,
# 不扫它就会出现"测试通过了、屏上却是空白"。扫这几个文件既完整,
# 也不会把注释里的中文误判成界面文本(只取字符串字面量)。
UI_SOURCES = (
    ROOT / "main" / "whale_ui.c",
    ROOT / "main" / "whale_ui.h",
    ROOT / "main" / "whale_model.c",
)

# LVGL cmap types (lv_font_fmt_txt_cmap_type_t). The converter emits the macro
# names, so both spellings are accepted when reading a generated file.
CMAP_TYPES = {
    "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY": 0,
    "LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL": 1,
    "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY": 2,
    "LV_FONT_FMT_TXT_CMAP_SPARSE_FULL": 3,
}
FORMAT0_TINY = 0
FORMAT0_FULL = 1
SPARSE_TINY = 2
SPARSE_FULL = 3


class Failure(Exception):
    pass


def strip_comments(source: str) -> str:
    """Remove // and /* */ comments while respecting string/char literals."""
    out = []
    i = 0
    n = len(source)
    while i < n:
        ch = source[i]
        nxt = source[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            while i < n and source[i] != "\n":
                i += 1
            continue
        if ch == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (source[i] == "*" and source[i + 1] == "/"):
                i += 1
            i += 2
            continue
        if ch == '"' or ch == "'":
            quote = ch
            out.append(ch)
            i += 1
            while i < n:
                if source[i] == "\\":
                    out.append(source[i])
                    i += 1
                    if i < n:
                        out.append(source[i])
                        i += 1
                    continue
                out.append(source[i])
                if source[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def string_literals(source: str) -> list[str]:
    return re.findall(r'"((?:[^"\\]|\\.)*)"', strip_comments(source))


def array_block(source: str, name: str) -> str:
    match = re.search(
        r"static const uint16_t " + re.escape(name) + r"\[\] = \{(.*?)\};", source, re.S
    )
    if match is None:
        raise Failure(f"font source does not define {name}[]")
    return match.group(1)


def parse_numbers(text: str) -> list[int]:
    values = []
    for token in text.split(","):
        token = token.strip()
        if not token:
            continue
        values.append(int(token, 16) if token.startswith("0x") else int(token))
    return values


def parse_cmaps(source: str) -> dict[int, int]:
    """Return {code point: glyph id} for every cmap entry in cmaps[]."""
    block_match = re.search(
        r"static const lv_font_fmt_txt_cmap_t cmaps\[\]\s*=\s*\{(.*?)\n\};", source, re.S
    )
    if block_match is None:
        raise Failure("font source does not define cmaps[]")
    block = block_match.group(1)

    covered: dict[int, int] = {}
    for raw in re.findall(r"\{(.*?)\}", block, re.S):
        def field(name: str) -> int:
            found = re.search(r"\." + name + r"\s*=\s*([^,]+)", raw)
            if found is None:
                raise Failure(f"cmap entry is missing .{name}")
            return int(found.group(1).strip(), 0)

        start = field("range_start")
        length = field("range_length")
        glyph_id_start = field("glyph_id_start")
        list_length = field("list_length")

        type_match = re.search(r"\.type\s*=\s*(\w+)", raw)
        if type_match is None:
            raise Failure("cmap entry is missing .type")
        type_token = type_match.group(1)
        if type_token in CMAP_TYPES:
            cmap_type = CMAP_TYPES[type_token]
        else:
            cmap_type = int(type_token, 0)
        list_name = re.search(r"\.unicode_list\s*=\s*(\w+)", raw)
        list_name = list_name.group(1) if list_name else None

        if cmap_type == FORMAT0_TINY or cmap_type == FORMAT0_FULL:
            offsets = range(length)
        elif cmap_type == SPARSE_TINY or cmap_type == SPARSE_FULL:
            if list_name is None or list_name == "NULL":
                raise Failure("sparse cmap entry has no unicode_list")
            offsets = parse_numbers(array_block(source, list_name))
            if len(offsets) != list_length:
                raise Failure(
                    f"{list_name}[] has {len(offsets)} entries but list_length is {list_length}"
                )
        else:
            raise Failure(f"unsupported cmap type {cmap_type}")

        for index, offset in enumerate(offsets):
            covered[start + offset] = glyph_id_start + index
    return covered


def parse_glyph_metrics(source: str) -> list[dict[str, int]]:
    match = re.search(
        r"static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc\[\] = \{(.*?)\n\};", source, re.S
    )
    if match is None:
        raise Failure("font source does not define glyph_dsc[]")
    metrics = []
    for raw in re.findall(r"\{([^{}]*)\}", match.group(1), re.S):
        entry = {}
        for name in ("bitmap_index", "adv_w", "box_w", "box_h", "ofs_x", "ofs_y"):
            found = re.search(r"\." + name + r"\s*=\s*(-?\d+)", raw)
            entry[name] = int(found.group(1)) if found else 0
        metrics.append(entry)
    return metrics


def read_font_metadata(source: str) -> tuple[int, int, int]:
    def value(name: str) -> int:
        found = re.search(r"\." + name + r"\s*=\s*(\d+)", source)
        if found is None:
            raise Failure(f"font descriptor is missing .{name}")
        return int(found.group(1))

    return value("line_height"), value("base_line"), value("bpp")


def check_size(size: int, inventory: list[str], used: dict[str, str]) -> tuple[int, int]:
    path = FONT_DIR / f"{FONT_NAME}_{size}.c"
    if not path.is_file():
        raise Failure(f"missing generated font source: {path.relative_to(ROOT)}")

    source = path.read_text(encoding="utf-8")
    covered = parse_cmaps(source)
    metrics = parse_glyph_metrics(source)
    line_height, base_line, bpp = read_font_metadata(source)

    # 1. 可打印 ASCII(数字、百分号、IP、时间都用得上)。
    missing_ascii = [cp for cp in range(0x20, 0x7F) if cp not in covered]
    if missing_ascii:
        raise Failure(
            f"{path.name} is missing ASCII code points: "
            + ", ".join(f"U+{cp:04X}" for cp in missing_ascii)
        )

    # 2. 字表里的每个字都要在位。
    missing = [ch for ch in inventory if ord(ch) not in covered]
    if missing:
        raise Failure(
            f"{path.name} is missing inventory characters: "
            + ", ".join(f"{ch} (U+{ord(ch):04X})" for ch in missing)
        )

    # 3. 界面字符串里真正用到的字也要在位。
    uncovered = {ch: where for ch, where in used.items() if ord(ch) not in covered}
    if uncovered:
        raise Failure(
            f"{path.name} cannot draw characters used in the UI: "
            + ", ".join(f"{ch} (U+{ord(ch):04X}, {where})" for ch, where in uncovered.items())
        )

    # 4. 汉字必须有真实的全宽字形(能抓到转换器输出空位图的情况)。
    bad_metrics = []
    short_glyphs = []
    full_height = 0
    ideographs = [cp for cp in sorted(covered) if 0x4E00 <= cp <= 0x9FFF]
    for cp in ideographs:
        glyph = metrics[covered[cp]]
        if glyph["adv_w"] < 240 or glyph["box_w"] < 12 or glyph["box_h"] < 1:
            bad_metrics.append(
                f"{chr(cp)} (U+{cp:04X}) adv_w={glyph['adv_w']} "
                f"box={glyph['box_w']}x{glyph['box_h']}"
            )
        elif glyph["box_h"] < 12:
            short_glyphs.append(f"{chr(cp)} (U+{cp:04X}, h={glyph['box_h']})")
        else:
            full_height += 1
    if bad_metrics:
        raise Failure(f"{path.name} has CJK glyphs with implausible metrics: "
                      + "; ".join(bad_metrics))
    if ideographs and full_height * 10 < len(ideographs) * 9:
        raise Failure(
            f"{path.name}: too many CJK glyphs do not fill the em box: "
            + ", ".join(short_glyphs)
        )

    print(
        f"  {path.name}: {len(covered)} glyphs, bpp={bpp}, "
        f"line_height={line_height}, base_line={base_line}"
    )
    return len(covered), len([cp for cp in covered if cp > 0x7F])


def main() -> int:
    if not INVENTORY.is_file():
        raise Failure(f"missing character inventory: {INVENTORY.relative_to(ROOT)}")

    inventory = [ch for ch in dict.fromkeys(INVENTORY.read_text(encoding="utf-8").strip())]
    if not inventory:
        raise Failure("character inventory is empty; run tools/gen_whale_font.py")

    used: dict[str, str] = {}
    missing_sources = [str(p) for p in UI_SOURCES if not p.is_file()]
    if missing_sources:
        raise Failure("missing UI source(s): " + ", ".join(missing_sources))
    for path in UI_SOURCES:
        for literal in string_literals(path.read_text(encoding="utf-8")):
            for ch in literal:
                if ord(ch) > 0x7F:
                    used.setdefault(ch, path.name)

    if not used:
        raise Failure("no Chinese text found in the UI sources; scan configuration is wrong")

    for size in SIZES:
        check_size(size, inventory, used)

    print(
        "Whale font coverage: PASS "
        f"({len(inventory)} inventory characters, {len(used)} used in UI, sizes {SIZES})"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
