#!/usr/bin/env python3
"""换台耗时探针：用串口 'n' 命令换台，量"触发 → 出声"的真实耗时。

用法:
    python3 probe_switch.py [端口] [轮数]

依赖固件日志：`[play] <台号>/<总数> <台名>  出声 <毫秒>ms`
"""
import sys, time, re, serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem21101"
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 4

ser = serial.Serial(port, 115200, timeout=0.05)
try:
    ser.setDTR(False)
    ser.setRTS(False)
except Exception:
    pass

buf = []

def pump(seconds, tag=""):
    t_end = time.time() + seconds
    while time.time() < t_end:
        line = ser.readline()
        if not line:
            continue
        s = line.decode("utf-8", "replace").rstrip()
        if s:
            print(f"{tag}{s}")
            buf.append(s)

print(f"=== 打开 {port}，先静置 3 秒看当前状态 ===")
pump(3.0)

for r in range(rounds):
    print(f"\n=== 第 {r+1} 轮：发送 n 换台 ===")
    buf.clear()
    ser.write(b"n")
    pump(6.0)
    hit = [m for m in buf if "出声" in m]
    if hit:
        m = re.search(r"出声 (\d+)ms", hit[-1])
        print(f"--> 出声 {m.group(1) if m else '?'}ms   {hit[-1]}")
    else:
        print("--> 本轮无出声日志（可能台源失效，已自动跳台）")
    time.sleep(0.4)

ser.close()
print("\n=== 结束 ===")
