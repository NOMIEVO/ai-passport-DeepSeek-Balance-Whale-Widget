#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把参考挂件的角色图 assets/img/DSniang1.png 转成 LVGL 9 的 C 数组。

来源图是 DeepSeek-Balance-Whale-Widget 的原版资源(610x610 透明底 cut-out),
不是本项目自生成;这里只做缩放 + RGB565A8 编码。

格式:LV_COLOR_FORMAT_RGB565A8 —— 前 W*H*2 字节是小端 RGB565 颜色面,
紧跟 W*H 字节的 A8 透明度面(边缘抗锯齿保留 8 bit 全量 alpha)。

用法:python tools/gen_whale_img.py [--size 106] [--ascii]
输出:assets/img/whale_dsnang_img.c(--ascii 时另打印亮度预览到 stdout)
"""
import argparse
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "assets" / "img" / "DSniang1.png"
OUT = REPO / "assets" / "img" / "whale_dsnang_img.c"
NAME = "whale_dsnang_img"
DEFAULT_SIZE = 139          # 主屏角色图边长;改了要同步 whale_ui.c 的 WHALE_IMG_S


def prepare(size):
    """原图 → 去掉透明边距 → 补成正方形 → 缩到 size。

    DSniang1.png 的实际内容框是 (45,10)-(610,610),不裁的话左上角会有一圈空白,
    放进有限的屏幕高度里等于白占像素;裁完再把画面补成正方形(保留宽高比),
    缩到目标尺寸时角色才能顶满。
    """
    img = Image.open(SRC).convert("RGBA")
    img = img.crop(img.getchannel("A").getbbox())
    w, h = img.size
    side = max(w, h)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(img, ((side - w) // 2, (side - h) // 2))
    return canvas.resize((size, size), Image.LANCZOS)


def rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=DEFAULT_SIZE)
    ap.add_argument("--ascii", action="store_true", help="打印 ASCII 亮度预览")
    args = ap.parse_args()
    size = args.size

    img = prepare(size)
    px = img.load()

    blob = bytearray()
    for y in range(size):
        for x in range(size):
            r, g, b, a = px[x, y]
            c = rgb565(r, g, b)
            blob += bytes((c & 0xFF, c >> 8))
    for y in range(size):
        for x in range(size):
            blob.append(px[x, y][3])

    # ---- ASCII 预览(alpha*亮度映射,和按钮库的验收方式一致) ----
    if args.ascii:
        ramp = " .:-=+*#%@"
        for yy in range(0, size, 2):
            line = ""
            for xx in range(size):
                r, g, b, a = px[xx, yy]
                lum = (r * 299 + g * 587 + b * 114) // 1000
                idx = min((lum * a) * len(ramp) // (256 * 256), len(ramp) - 1)
                line += ramp[idx]
            print(line)

    stride = size * 2
    out = [
        "// 自动生成:tools/gen_whale_img.py —— 不要手改,改脚本后重新生成。",
        "// 来源:assets/img/DSniang1.png(参考挂件 DeepSeek-Balance-Whale-Widget 原版资源,",
        "// 610x610 透明底 cut-out,此处缩放为 %dx%d)。" % (size, size),
        "// 布局:RGB565A8 = 前 %d 字节小端颜色面 + 紧跟 %d 字节 A8 透明度面。"
        % (size * size * 2, size * size),
        '#include "lvgl.h"',
        "",
        "const uint8_t %s_map[%d] __attribute__((aligned(4))) = {" % (NAME, len(blob)),
    ]
    for i in range(0, len(blob), 12):
        out.append("    " + ", ".join("0x%02x" % b for b in blob[i:i + 12]) + ",")
    out += [
        "};",
        "",
        "const lv_image_dsc_t %s = {" % NAME,
        "    .header = {",
        "        .magic = LV_IMAGE_HEADER_MAGIC,",
        "        .cf = LV_COLOR_FORMAT_RGB565A8,",
        "        .flags = 0,",
        "        .w = %d," % size,
        "        .h = %d," % size,
        "        .stride = %d," % stride,
        "    },",
        "    .data_size = sizeof(%s_map)," % NAME,
        "    .data = %s_map," % NAME,
        "};",
        "",
    ]
    OUT.write_text("\n".join(out), encoding="utf-8")
    print("wrote %s (%d bytes map, %dx%d RGB565A8)" % (OUT, len(blob), size, size))


if __name__ == "__main__":
    main()
