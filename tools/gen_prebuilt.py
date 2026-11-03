#!/usr/bin/env python3
"""把 arduino-cli 的编译产物合成两种可直接烧的镜像。

输出（../prebuilt/）：
  Dayu-radio-src-flash.bin   从 0x0 起的最小镜像（bootloader + 分区表 +
                             boot_app0 + app，尾巴按 64KB 对齐）
  Dayu-radio-src-16M.bin     整片 16MB（未用区域全部 0xFF）

布局（arduino-cli 的 merged.bin 已经是这个布局，这里只是重新打包并核对）：
  0x000000 bootloader
  0x008000 分区表
  0x00E000 boot_app0
  0x010000 应用

编译要用固定的 --build-path，产物才会落在这里：
  arduino-cli compile --fqbn "$FQBN" --build-path build .
"""
import hashlib, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.normpath(os.path.join(HERE, ".."))
SKETCH = os.path.join(PROJ, "dayu_radio")
BUILD = os.path.join(SKETCH, "build")
OUT = os.path.join(PROJ, "prebuilt")

BOOT = os.path.join(BUILD, "dayu_radio.ino.bootloader.bin")
PART = os.path.join(BUILD, "dayu_radio.ino.partitions.bin")
APP = os.path.join(BUILD, "dayu_radio.ino.bin")
BOOT_APP0 = os.path.expanduser(
    "~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.12/tools/partitions/boot_app0.bin")

for p in (BOOT, PART, APP, BOOT_APP0):
    if not os.path.exists(p):
        sys.exit(f"缺文件：{p}")

img = bytearray(b"\xff" * (16 * 1024 * 1024))


def put(path, off):
    data = open(path, "rb").read()
    img[off:off + len(data)] = data
    print(f"  {os.path.basename(path):42s} @0x{off:06X}  {len(data):>9,} B")
    return off + len(data)


print("布局：")
put(BOOT, 0x000000)
put(PART, 0x008000)
put(BOOT_APP0, 0x00E000)
app_end = put(APP, 0x010000)

# 分区表里 app0 是 0x300000，写超了开机就起不来，这里直接拦住
APP_LIMIT = 0x300000
if os.path.getsize(APP) > APP_LIMIT:
    sys.exit(f"\n✗ app {os.path.getsize(APP):,} B 超过 app0 分区 {APP_LIMIT:,} B，烧了也起不来")
print(f"\napp 占分区 {os.path.getsize(APP) * 100 / APP_LIMIT:.1f}%"
      f"（{(APP_LIMIT - os.path.getsize(APP)):,} B 余量）")

# 最小镜像：到 app 结束，再按 64KB 向上取整
trim = (app_end + 0xFFFF) & ~0xFFFF
os.makedirs(OUT, exist_ok=True)

f1 = os.path.join(OUT, "Dayu-radio-src-flash.bin")
open(f1, "wb").write(bytes(img[:trim]))

f2 = os.path.join(OUT, "Dayu-radio-src-16M.bin")
open(f2, "wb").write(bytes(img))

print(f"\napp 结束 0x{app_end:06X}，最小镜像取到 0x{trim:06X}\n")
for f in (f1, f2):
    d = open(f, "rb").read()
    print(f"{os.path.basename(f):34s} {len(d):>10,} B  sha256 {hashlib.sha256(d).hexdigest()}")
