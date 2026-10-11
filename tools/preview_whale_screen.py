#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把挂件主屏按比例渲染成 PNG,刷机前先看排版。

省刷机次数:布局/尺寸改动先在 PC 上按真实像素比例看一眼,确认气泡有没有压住角色、
文本有没有越过椭圆描边、离边框够不够。

三样东西都按固件里的实际来:
  · 气泡 —— 走 tools/gen_whale_bubble.py 的同一套几何(render_chain),不是近似图形;
  · 角色 —— assets/img/DSniang1.png,裁透明边距后按 WHALE_IMG_S 缩放;
  · 几何常量 —— 从 main/whale_ui.c 的 #define 里读,避免两处不一致(读不到用默认值)。

用法:python tools/preview_whale_screen.py [--out build/preview.png] [--scale 3]
"""
import argparse
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
UI = REPO / "main" / "whale_ui.c"
IMG = REPO / "assets" / "img" / "DSniang1.png"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_whale_bubble import render_chain  # noqa: E402  同一套气泡几何

# 屏幕与配色(与 whale_ui.c 保持一致)
SCREEN_W, SCREEN_H = 240, 320
CORNER = 30            # BSP 把四角裁成半径 30 的圆角
C_BG = 0x0B1330
C_INK = 0x203170
C_BUBBLE = 0xFFFFFF
C_TOP = 0x8FA0C0
C_DIM = 0x7C8CB0
C_VALLEY = 0x2ECC9B
C_GOLD = 0xFFC93C

DEFAULTS = {
    "BUB_IMG_W": 194, "BUB_IMG_X": 16, "BUB_IMG_Y": 28,
    "BUB_ELL_CX": 115, "BUB_ELL_CY": 91, "BUB_ELL_RX": 97, "BUB_ELL_RY": 60,
    "BUB_TEXT_X": 45, "BUB_TEXT_W": 140,
    "BUB_ROW1_Y": 50, "BUB_ROW2_Y": 76, "BUB_ROW3_Y": 114,
    "WHALE_IMG_S": 139, "WHALE_IMG_RIGHT_GAP": 6, "WHALE_BOTTOM": 262,
    "STATUS_Y": 266, "HINT_Y": 288, "LINE_H": 18, "BADGE_W": 44,
    "TOP_Y": 6, "TOP_H": 18, "MARGIN": 18, "COIN_X": 64,
}


def load_consts():
    """抽取 whale_ui.c 里的整数 #define(够简单的才认)。"""
    src = UI.read_text(encoding="utf-8")
    out = dict(DEFAULTS)
    for name, value in re.findall(r"^#define\s+([A-Z0-9_]+)\s+(\d+)\s*$", src, re.M):
        out[name] = int(value)
    return out


def prepare_art(size):
    art = Image.open(IMG).convert("RGBA")
    art = art.crop(art.getchannel("A").getbbox())
    w, h = art.size
    side = max(w, h)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(art, ((side - w) // 2, (side - h) // 2))
    return canvas.resize((size, size), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(REPO / "build" / "preview.png"))
    ap.add_argument("--scale", type=int, default=3, help="放大倍数,方便看细节")
    args = ap.parse_args()

    c = load_consts()
    img_s = c["WHALE_IMG_S"]
    art_x = SCREEN_W - img_s - c["WHALE_IMG_RIGHT_GAP"]
    art_y = c["WHALE_BOTTOM"] - img_s

    screen = Image.new("RGB", (SCREEN_W, SCREEN_H), hexcolor(C_BG))
    d = ImageDraw.Draw(screen)

    # 顶栏:标题占位条 + 右侧电量占位
    d.rounded_rectangle([c["MARGIN"], c["TOP_Y"], 120, c["TOP_Y"] + c["TOP_H"]],
                        radius=4, fill=hexcolor(C_TOP))
    d.rounded_rectangle([SCREEN_W - 18 - 58, c["TOP_Y"], SCREEN_W - 18, c["TOP_Y"] + c["TOP_H"]],
                        radius=4, fill=hexcolor(C_TOP))

    # 角色先画 —— 参考挂件里 img 在 bubble 之前,气泡是压在角色身上的。
    art = prepare_art(img_s)
    screen.paste(art, (art_x, art_y), art)

    # 气泡整条(大椭圆 + 半椭圆尾巴 + 两个小泡泡)
    chain = render_chain(c["BUB_IMG_W"])
    screen.paste(chain, (c["BUB_IMG_X"], c["BUB_IMG_Y"]), chain)

    # 三行文本占位(用真实的行位与宽度,能看出来是否越过椭圆)
    for key, h in (("BUB_ROW1_Y", 18), ("BUB_ROW2_Y", 30), ("BUB_ROW3_Y", 18)):
        y = c[key]
        d.rounded_rectangle([c["BUB_TEXT_X"], y + 3,
                             c["BUB_TEXT_X"] + c["BUB_TEXT_W"], y + h - 3],
                            radius=3, fill=hexcolor(C_INK))

    # 状态行:左侧峰/谷徽章 + 文本占位;底部提示行
    d.rounded_rectangle([c["MARGIN"], c["STATUS_Y"], c["MARGIN"] + c["BADGE_W"],
                         c["STATUS_Y"] + c["LINE_H"]], radius=9, fill=hexcolor(C_VALLEY))
    d.rounded_rectangle([c["MARGIN"] + c["BADGE_W"] + 6, c["STATUS_Y"] + 3, 200,
                         c["STATUS_Y"] + c["LINE_H"] - 3], radius=3, fill=hexcolor(C_DIM))
    d.rounded_rectangle([c["MARGIN"], c["HINT_Y"] + 3, 190, c["HINT_Y"] + c["LINE_H"] - 3],
                        radius=3, fill=hexcolor(C_DIM))

    # 金币落点(消费时从 y=0 掉到 y=72,这里画最终位置)
    coin_x = c["COIN_X"]
    d.ellipse([coin_x, art_y + 72, coin_x + 16, art_y + 88], fill=C_GOLD)

    # 四角圆角遮罩:圆角外画透明,模拟 BSP 的裁切
    mask = Image.new("L", (SCREEN_W, SCREEN_H), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, SCREEN_W - 1, SCREEN_H - 1],
                                           radius=CORNER, fill=255)
    screen.putalpha(mask)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    if args.scale > 1:
        screen = screen.resize((SCREEN_W * args.scale, SCREEN_H * args.scale), Image.NEAREST)
    screen.convert("RGB").save(out)
    print("wrote %s (chain %dx%d at %d,%d; art %dx%d at %d,%d)"
          % (out, chain.size[0], chain.size[1], c["BUB_IMG_X"], c["BUB_IMG_Y"],
             img_s, img_s, art_x, art_y))


def hexcolor(v):
    return (v >> 16 & 0xFF, v >> 8 & 0xFF, v & 0xFF)


if __name__ == "__main__":
    main()
