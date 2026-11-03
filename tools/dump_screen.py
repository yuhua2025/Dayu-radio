#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 Dayu-radio 的屏幕内容抓下来存成 PNG（不用相机核版式）。

原理：固件里 uiDump() 会把离屏 sprite 按 RGB565 原样吐到串口，
      夹在 "DUMP 240 240" 和 "ENDDUMP" 之间。

用法：
  python3 dump_screen.py <串口> <输出.png> [--pre k] [--scale 2]

  --pre   在 dump 之前先发一个字符。k = 冻住开机画面预览，n = 换台
  --scale 放大倍数（默认 2，240x240 直接看太小）

注意：一帧 115200 字节，115200 波特率下大约要 10 秒，属正常。
"""
import argparse, sys, time
import serial
from PIL import Image

W = H = 240
BYTES = W * H * 2
MARKER = b"DUMP %d %d " % (W, H)      # 固件在头部之后直接跟二进制，不带换行


def grab(port, pre=None, timeout=40):
    ser = serial.Serial(port, 115200, timeout=0.2)
    time.sleep(0.4)
    ser.reset_input_buffer()

    if pre:
        ser.write(pre.encode())
        ser.flush()
        time.sleep(0.6)

    ser.write(b"d")
    ser.flush()

    buf = bytearray()
    started = False
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            if not started:
                i = buf.find(MARKER)
                if i >= 0:
                    started = True
                    del buf[:i + len(MARKER)]     # 精确切到二进制起点
            if started and len(buf) >= BYTES:
                break
        else:
            time.sleep(0.01)
    ser.close()

    if not started:
        sys.exit("没等到 DUMP 头，检查串口是不是被别的程序占着")
    if len(buf) < BYTES:
        sys.exit(f"只收到 {len(buf)}/{BYTES} 字节")
    return bytes(buf[:BYTES])


def to_image(data, scale=2):
    img = Image.new("RGB", (W, H))
    pix = []
    for i in range(0, BYTES, 2):
        # ★ 大端。spr.readRect() 在"目标色深和 sprite 一样"时走的是直接 memcpy
        #   (LGFX_Sprite.inl:553)，吐出来的是 sprite 内部按面板顺序存的字节，
        #   也就是高字节在前。按小端解会得到一堆错色（背景变土黄、底部一条花屏）。
        v = (data[i] << 8) | data[i + 1]
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        pix.append((r * 255 // 31, g * 255 // 63, b * 255 // 31))
    img.putdata(pix)
    if scale != 1:
        img = img.resize((W * scale, H * scale), Image.NEAREST)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("out")
    ap.add_argument("--pre", default=None)
    ap.add_argument("--scale", type=int, default=2)
    a = ap.parse_args()

    data = grab(a.port, a.pre)
    img = to_image(data, a.scale)
    img.save(a.out)
    print(f"已保存 {a.out}  ({img.width}x{img.height})")


if __name__ == "__main__":
    main()
