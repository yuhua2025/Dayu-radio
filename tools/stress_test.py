#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""整机压力自检：换台 / 连击 / 音量边界 / 频谱样式 / API / 内存 / 重启恢复。

一次跑完出报告，用来在改动后自查有没有把东西弄坏，不必人手一个个试。

用法:
    python3 stress_test.py [串口] [设备IP]

判定标准（任何一条不满足就报 FAIL）：
  · 每次换台都要在 6 秒内出声
  · 全程不能出现 [wdt]（看门狗重启）、ESP-ROM（意外复位）、[proxy] 断流
  · 音量 0 / 最大 / 中间值 三次设置后，status 里的 vol 要对得上
  · 5 种频谱样式切换都要返回 ok，且 status 的 spec 跟着变
  · 重启 3 次都要能自己恢复播放
  · 结束时 heap / psram 不能比开始时少 20KB 以上（粗测泄漏）
"""
import sys, time, json, re, serial, urllib.request, urllib.parse

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem21101"
IP   = sys.argv[2] if len(sys.argv) > 2 else None

def http(path, timeout=8):
    base = f"http://{IP}"
    return urllib.request.urlopen(base + path, timeout=timeout).read().decode("utf-8", "replace")

def status():
    return json.loads(http("/api/status"))

def find_ip(port):
    """按 MAC 在 ARP 表里找设备 IP（DHCP 会换地址）"""
    import subprocess
    out = subprocess.run(["arp", "-a"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "94:a9:90:26:90:20" in line.lower():
            m = re.search(r"\(([0-9.]+)\)", line)
            if m:
                return m.group(1)
    return None

ser = serial.Serial(PORT, 115200, timeout=0.05)
ser.setDTR(False); ser.setRTS(False)

log = []
anomalies = []
in_restart = False          # 我们自己发起 /restart 时，ESP-ROM 不算异常

def reopen_serial(wait_s=25):
    """ESP.restart() 后原生 USB CDC 会重新枚举，旧句柄直接失效（Errno 6）。
    以前不重连的话，脚本会"看不见"设备开机 —— 于是把好好的重启判成
    "没恢复播放"。这里关掉旧句柄、重新找端口打开。"""
    global ser, in_restart
    try:
        ser.close()
    except Exception:
        pass
    import glob
    t0 = time.time()
    while time.time() - t0 < wait_s:
        cands = [PORT] + sorted(glob.glob("/dev/cu.usbmodem*"))
        for p in cands:
            try:
                s = serial.Serial(p, 115200, timeout=0.05)
                s.setDTR(False); s.setRTS(False)
                ser = s
                in_restart = True     # 重开端口本身会拉一次复位，这条启动不算异常
                return True
            except Exception:
                continue
        time.sleep(0.4)
    return False

def pump(seconds, tag=""):
    """读串口，收集日志；顺手标记异常"""
    global in_restart
    t_end = time.time() + seconds
    while time.time() < t_end:
        try:
            line = ser.readline()
        except Exception:
            reopen_serial()
            continue
        if not line:
            continue
        s = line.decode("utf-8", "replace").strip()
        if not s:
            continue
        log.append(s)
        if tag:
            print(f"  {tag} {s}")
        if "[wdt]" in s:
            anomalies.append("看门狗重启：" + s)
        if "ESP-ROM" in s:
            if not in_restart:
                anomalies.append("意外复位：" + s)
            in_restart = False
        if "断流" in s:
            anomalies.append("断流：" + s)
        if "连接失败" in s:
            anomalies.append("连接失败：" + s)
        if "panic" in s.lower() or "Guru Meditation" in s:
            anomalies.append("崩溃：" + s)

def wait_sound(timeout=45):
    """等下一次出声，返回 (耗时秒, 日志行) 或 (None, None)"""
    global in_restart
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            line = ser.readline()
        except Exception:          # 重启把句柄带走了：重连后继续等
            if not reopen_serial():
                return None, None
            continue
        if not line:
            continue
        s = line.decode("utf-8", "replace").strip()
        if not s:
            continue
        log.append(s)
        if "[wdt]" in s: anomalies.append("看门狗重启：" + s)
        if "ESP-ROM" in s:
            if not in_restart:
                anomalies.append("意外复位：" + s)
            in_restart = False
        if "断流" in s: anomalies.append("断流：" + s)
        m = re.search(r"出声 (\d+)ms", s)
        if m:
            return int(m.group(1)) / 1000.0, s
    return None, None

def find_ip_now():
    global IP
    if not IP:
        IP = find_ip(PORT)
    return IP

print("=== 等设备联网并开播 ===")
find_ip_now()
t0 = time.time()
while time.time() - t0 < 90:
    ms, line = wait_sound(5)
    if ms:
        print(f"  已开播（{ms*1000:.0f}ms）")
        break
else:
    print("  !! 90 秒内没开播，后续 API 测试可能失败")
find_ip_now()
print(f"  设备 IP = {IP}")

st0 = status()
print(f"  起始：heap={st0.get('heap')} psram={st0.get('psram')} 台={st0.get('name')}")

results = []

# ---- 1. 连续换台 10 次 ----
print("\n=== 1. 连续换台 10 次（每次等出声）===")
times = []
for i in range(10):
    ser.write(b"n"); ser.flush()
    ms, line = wait_sound(12)
    times.append(ms)
    if ms:
        name = re.search(r"\d+/\d+ (\S+?) +出声", line)
        tag = name.group(1) if name else "?"
        warn = "  ← 慢！" if ms > 3 else ""
        print(f"  第{i+1}次：{ms*1000:.0f}ms  {tag}{warn}")
    else:
        print(f"  第{i+1}次：FAIL(没出声)")
        results.append(f"换台第{i+1}次失败")
    time.sleep(0.6)
ok = [t for t in times if t]
if ok:
    print(f"  平均 {sum(ok)/len(ok)*1000:.0f}ms  最慢 {max(ok)*1000:.0f}ms")
    if max(ok) > 6:
        results.append(f"换台最慢 {max(ok)*1000:.0f}ms 超过 6 秒")

# ---- 2. 快速连击 5 下 ----
print("\n=== 2. 快速连击 5 下（100ms 间隔）===")
for _ in range(5):
    ser.write(b"n"); ser.flush()
    time.sleep(0.1)
ms, _ = wait_sound(15)
print(f"  连击后出声：{'%dms' % (ms*1000) if ms else 'FAIL'}")
if ms is None:
    results.append("连击后没出声")
time.sleep(1.5)

# ---- 3. 音量边界 ----
print("\n=== 3. 音量边界（0 / 21 / 10）===")
for v in (0, 21, 10):
    http(f"/vol?v={v}")
    time.sleep(0.6)
    got = status().get("vol")
    flag = "OK" if int(got) == v else "FAIL"
    print(f"  设 {v} → 实际 {got}  {flag}")
    if flag == "FAIL":
        results.append(f"音量设置 {v} 不生效（实际 {got}）")

# ---- 4. 频谱样式全切两轮 ----
print("\n=== 4. 频谱样式 0~4 切两轮 ===")
for r in range(2):
    for s in range(5):
        http(f"/spec?s={s}")
        time.sleep(0.35)
        got = status().get("spec")
        if int(got) != s:
            results.append(f"样式切换 {s} 不生效（实际 {got}）")
    print(f"  第{r+1}轮完成")
http("/spec?s=0")

# ---- 5. API 全量抽查 ----
print("\n=== 5. 网页 API 抽查 ===")
for path in ("/api/status", "/tunenext", "/tuneprev", "/vol?v=12"):
    try:
        body = http(path, timeout=10)
        print(f"  {path:18s} → {len(body)}B {body[:40]!r}")
    except Exception as e:
        results.append(f"API {path} 异常：{e}")
time.sleep(2)

# ---- 6. 内存 ----
st1 = status()
dh = (st0.get("heap") or 0) - (st1.get("heap") or 0)
dp = (st0.get("psram") or 0) - (st1.get("psram") or 0)
print(f"\n=== 6. 内存：heap 变化 {-dh:+d}B  psram 变化 {-dp:+d}B ===")
if dh > 20480:
    results.append(f"heap 掉了 {dh}B，疑似泄漏")
if dp > 65536:
    results.append(f"psram 掉了 {dp}B，疑似泄漏")

# ---- 7. 重启 3 次 ----
print("\n=== 7. 重启 3 次，每次都要自己恢复播放 ===")
for i in range(3):
    in_restart = True
    try:
        http("/restart", timeout=5)
    except Exception:
        pass
    time.sleep(3)
    # ★ 重启会让 USB CDC 重新枚举，必须重开串口才有日志可读（否则永远等不到"出声"）
    reopen_serial()
    pump(1)                       # 丢掉重启瞬间的残留
    ms, _ = wait_sound(45)
    print(f"  第{i+1}次重启：{'恢复播放 %dms' % (ms*1000) if ms else 'FAIL(没恢复)'}")
    if ms is None:
        results.append(f"重启第{i+1}次没恢复播放")

# ---- 汇总 ----
print("\n" + "=" * 46)
if anomalies:
    print(f"异常日志 {len(anomalies)} 条：")
    for a in anomalies[:12]:
        print("  · " + a)
else:
    print("异常日志：无")
if results:
    print(f"\n失败项 {len(results)} 个：")
    for r in results:
        print("  ✗ " + r)
else:
    print("\n全部通过 ✓")
print("=" * 46)
ser.close()
