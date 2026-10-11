#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""照参考挂件的气泡 SVG 几何画出白色对话气泡,编成 LVGL 9 的 C 数组。

参考挂件的气泡是代码画的 SVG(whale-widget-prompt.md 里有原始几何),画布
1026x700、描边 #203170 宽 18:

    大椭圆      中心 (454,247) rx373 ry232     —— 注意是**真椭圆**,不是圆角矩形
    尾巴半椭圆  端点 (301,465)-(413,484),中心 (356,472),倾 10°
    小气泡1     中心 (352,561) rx37.5 ry26
    小气泡2     中心 (442,646) rx24.5 ry18

这三个geometrie 相对位置照抄,只整体等比缩放放大到名片屏幕宽度。之前用圆角矩形
拼的版本是"胶囊形",现在是"椭圆",这才是 assets/DSniang02.png 里的样子。

为什么偏要生成图片:ESP-IDF 里的 LVGL 没开矢量引擎(CONFIG_LV_USE_VECTOR_GRAPHIC
未设),而圆角矩形改成 pill 出来的是胶囊不是椭圆。把整条气泡(大椭圆 + 尾巴 +
两个小气泡)按上述几何 raster 成 RGB565A8,形状才能和参考一致;文字仍用 LVGL
标签压在图片上层,布局和以前一样是独立的。

用法:
    python tools/gen_whale_bubble.py [--width 194] [--ascii]
输出:
    assets/img/whale_bubble_img.c
"""
import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "assets" / "img" / "whale_bubble_img.c"
NAME = "whale_bubble_img"
DEFAULT_WIDTH = 194        # 大椭圆在屏幕上的宽度;改了要同步 whale_ui.c 的 BUB_IMG_W

# ---- 参考挂件的原始几何(画布 1026 x 700) --------------------------------
REF_MAIN = dict(cx=454.0, cy=247.0, rx=373.0, ry=232.0)
REF_BRIDGE = dict(cx=356.0, cy=472.0, rx=56.8, ry=26.0, rot=10.0)   # 端点见模块文档
REF_B1 = dict(cx=352.0, cy=561.0, rx=37.5, ry=26.0)
REF_B2 = dict(cx=442.0, cy=646.0, rx=24.5, ry=18.0)
REF_STROKE = 18.0
# 四条曲线合起来再加描边后的内容框,用来定画布大小(左上/右下各留半条描边)。
REF_X0, REF_Y0 = 72.0, 6.0
REF_X1, REF_Y1 = 836.0, 673.0

WHITE = (255, 255, 255, 255)
INk = (0x20, 0x31, 0x70, 0xFF)


def chain_size(width: int):
    """给定大椭圆宽度,返回 (画布w, 画布h, 缩放比)。"""
    s = width / (REF_MAIN["rx"] * 2)
    return math.ceil((REF_X1 - REF_X0) * s), math.ceil((REF_Y1 - REF_Y0) * s), s


def _rotated_ellipse(rx, ry, stroke, rot_deg):
    """斜着放的椭圆只能先画够大的临时图再转:返回 (图, 半幅, 半幅)。"""
    pad = math.ceil(max(rx, ry) + stroke)
    tmp = Image.new("RGBA", (pad * 2, pad * 2), (0, 0, 0, 0))
    ImageDraw.Draw(tmp).ellipse([pad - rx, pad - ry, pad + rx, pad + ry],
                                fill=WHITE, outline=INk, width=stroke)
    tmp = tmp.rotate(-rot_deg, resample=Image.BICUBIC, center=(pad, pad))
    return tmp, pad


def render_chain(width: int, ss: int = 4) -> Image.Image:
    """整条气泡渲成一张 RGBA。ss 是超采样倍数,缩回来得到平滑抗锯齿。"""
    w, h, s = chain_size(width)
    big = Image.new("RGBA", (w * ss, h * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    stroke = max(1, round(REF_STROKE * s * ss))

    def put(shape, rot=0.0):
        rx, ry = shape["rx"] * s * ss, shape["ry"] * s * ss
        cx = (shape["cx"] - REF_X0) * s * ss
        cy = (shape["cy"] - REF_Y0) * s * ss
        if rot:
            tmp, pad = _rotated_ellipse(rx, ry, stroke, rot)
            big.alpha_composite(tmp, (round(cx - pad), round(cy - pad)))
        else:
            d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=WHITE,
                      outline=INk, width=stroke)

    # 顺序有讲究:尾巴画在大椭圆之前,接缝被大椭圆的白底盖住,露出来的就是
    # 从泡泡底伸向小气泡的那一段 —— 参考挂件也是这个叠法。
    put(REF_BRIDGE, REF_BRIDGE["rot"])
    put(REF_MAIN)
    put(REF_B1)
    put(REF_B2)

    return big.resize((w, h), Image.LANCZOS)


def ascii_preview(img: Image.Image, every: int = 2) -> None:
    ramp = " .:-=+*#%@"
    px = img.load()
    for y in range(0, img.size[1], every):
        line = ""
        for x in range(img.size[0]):
            r, g, b, a = px[x, y]
            lum = (r * 299 + g * 587 + b * 114) // 1000
            idx = min((lum * a) * len(ramp) // (256 * 256), len(ramp) - 1)
            line += ramp[idx]
        print(line)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    ap.add_argument("--ascii", action="store_true")
    args = ap.parse_args()

    img = render_chain(args.width)
    w, h = img.size
    px = img.load()
    if args.ascii:
        ascii_preview(img)

    blob = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, _ = px[x, y]
            c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            blob += bytes((c & 0xFF, c >> 8))
    for y in range(h):
        for x in range(w):
            blob.append(px[x, y][3])

    lines = [
        "// 自动生成:tools/gen_whale_bubble.py —— 不要手改,改脚本后重新生成。",
        "// 几何照抄参考挂件 DeepSeek-Balance-Whale-Widget 的气泡 SVG(画布 1026x700、",
        "// 描边 #203170 宽 18):大椭圆 + 半椭圆尾巴 + 两个小气泡,等比缩放到名片宽度。",
        "// 布局:RGB565A8 = 前 %d 字节小端颜色面 + 紧跟 %d 字节 A8 透明度面。"
        % (w * h * 2, w * h),
        '#include "lvgl.h"',
        "",
        "const uint8_t %s_map[%d] __attribute__((aligned(4))) = {" % (NAME, len(blob)),
    ]
    for i in range(0, len(blob), 12):
        lines.append("    " + ", ".join("0x%02x" % b for b in blob[i:i + 12]) + ",")
    lines += [
        "};",
        "",
        "const lv_image_dsc_t %s = {" % NAME,
        "    .header = {",
        "        .magic = LV_IMAGE_HEADER_MAGIC,",
        "        .cf = LV_COLOR_FORMAT_RGB565A8,",
        "        .flags = 0,",
        "        .w = %d," % w,
        "        .h = %d," % h,
        "        .stride = %d," % (w * 2),
        "    },",
        "    .data_size = sizeof(%s_map)," % NAME,
        "    .data = %s_map," % NAME,
        "};",
        "",
    ]
    OUT.write_text("\n".join(lines), encoding="utf-8")
    print("wrote %s (%d bytes map, %dx%d RGB565A8)" % (OUT, len(blob), w, h))


if __name__ == "__main__":
    main()
