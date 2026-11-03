#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成中文点阵字模（1bpp），供 dayu_radio 自绘使用。

为什么不用 u8g2 字体：
  u8g2_font_wqy16_t_gb2312 是 **GB2312 编码** 的字体，没有 Unicode 索引表；
  而 LovyanGFX 的 lgfx::U8g2font::getGlyph() 只认 Unicode 编码字体
  （它会从 header[21..22] 取 start_pos_unicode 当查找表起点）。
  拿 GB2312 字体去查汉字 → 查表读到垃圾数据 → 进入无界循环 → 主线程卡死，
  表现就是"背光亮、屏幕全黑"。

本脚本直接从系统 TTF 栅格化出 GB2312 全字集 + ASCII 的点阵，
输出纯 1bpp 位图 + 按 Unicode 排序的索引表，查表用二分，
命中不了就画方框（不会越界、不会死循环）。

用法：
  python3 gen_cnfont.py <输出目录>
"""
import sys, os
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/System/Library/Fonts/Hiragino Sans GB.ttc"
SS = 2  # 超采样倍数：按 2 倍尺寸渲染，再缩到目标尺寸，笔画更结实


def charset():
    """ASCII 可打印字符 + GB2312 全字集"""
    cps = [c for c in range(0x20, 0x7F)]
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            cps.append(ord(ch))
    # 去重 + 排序（二分查找要求有序）
    return sorted(set(cps))


def make_font(size):
    """返回 (cps, bits, advs)；bits 为 size*ceil(size/8) 字节/字的定长位图"""
    f = ImageFont.truetype(FONT_PATH, size * SS)
    asc, desc = f.getmetrics()
    H = asc + desc
    B = size - 3                      # 基线所在行（格子内）
    Y0 = SS * size + asc - SS * B     # 画布上的裁切起点 Y
    row_bytes = (size + 7) // 8
    glyph_bytes = row_bytes * size

    pad = SS * size * 2               # 画布留白，保证任何字形都装得下
    canvas_w = pad * 2
    canvas_h = pad * 2 + H

    cps, bits, advs = [], bytearray(), bytearray()
    for cp in charset():
        ch = chr(cp)
        img = Image.new("L", (canvas_w, canvas_h), 0)
        ImageDraw.Draw(img).text((pad, pad + asc), ch, font=f, fill=255, anchor="ls")

        cell = img.crop((pad, pad + asc - SS * B, pad + SS * size, pad + asc - SS * B + SS * size))
        cell = cell.resize((size, size), Image.LANCZOS)

        g = bytearray(glyph_bytes)
        px = cell.load()
        for yy in range(size):
            base = yy * row_bytes
            for xx in range(size):
                # 阈值 96：兼顾细笔画与不要糊成一片
                if px[xx, yy] >= 96:
                    g[base + (xx >> 3)] |= 0x80 >> (xx & 7)

        adv = round(f.getlength(ch) / SS)
        if cp > 0x2E80:               # CJK / 全角符号：占满一个字宽
            adv = size
        elif adv < 1:
            adv = size // 2
        elif adv > size:
            adv = size

        cps.append(cp)
        bits += g
        advs.append(adv)

    # 位图按行存，但要按"字"定长 → 已经是定长了
    return cps, bytes(bits), bytes(advs), glyph_bytes


def emit(path, name, cps, bits, advs, glyph_bytes, size):
    lines = []
    lines.append("// 自动生成，请勿手改 —— 由 tools/gen_cnfont.py 生成")
    lines.append("// 源字体：Hiragino Sans GB  尺寸：%dx%d  字数：%d  位图字节/字：%d"
                 % (size, size, len(cps), glyph_bytes))
    lines.append('#include "cnfont.h"')
    lines.append("")
    lines.append("const uint16_t %s_cps[] = {" % name)
    for i in range(0, len(cps), 12):
        lines.append("  " + ",".join("0x%04X" % c for c in cps[i:i + 12]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("const uint8_t %s_adv[] = {" % name)
    for i in range(0, len(advs), 24):
        lines.append("  " + ",".join(str(a) for a in advs[i:i + 24]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("const uint8_t %s_bits[] = {" % name)
    for i in range(0, len(bits), 32):
        lines.append("  " + ",".join("0x%02X" % b for b in bits[i:i + 32]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("const unsigned %s_count = %d;" % (name, len(cps)))
    lines.append("")
    with open(path, "w") as fp:
        fp.write("\n".join(lines))
    print("  %-28s %8d 字节源码, %d 字" % (os.path.basename(path), os.path.getsize(path), len(cps)))


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(outdir, exist_ok=True)
    # 16px = 辅助文字（歌曲名、配网说明、顶部小标），20px = 台名 / 开机大字。
    # 12px 在 1bpp 下横画必然丢失，反而更难认，所以不做。
    for size, name in ((16, "kCn16"), (20, "kCn20")):
        print("[gen] %dx%d" % (size, size))
        cps, bits, advs, gb = make_font(size)
        emit(os.path.join(outdir, "cnfont%d.c" % size), name, cps, bits, advs, gb, size)
    print("完成")


if __name__ == "__main__":
    main()
