#line 1 "/Users/dayu/WorkBuddy/2026-11-02-10-50-15/Dayu-radio/dayu_radio/stations.h"
// ============================================================================
//  stations.h —— 内置电台表（现在是空的，开机会直接用原厂那批蜻蜓FM 台）
//
//  开机列表的拼装顺序（见 dayu_radio.ino 的 buildList()）：
//    ① 原厂蜻蜓FM 台     stations-qtfm.h        62 个，实测可用
//    ② 本文件内置台      stations.h             ★ 已清空（USE_BUILTIN_STATIONS 0）
//    ③ 配网页面上填的台  NVS 的 "custom"        每行 "名称|URL"
//
//  ---- 为什么要清空 ----
//  原来预置的 9 个是"能连但内容不对口"的境外公共流（Radio Paradise / Radio Swiss /
//  KEXP 之类），国内还慢，跟原厂电台混在一起很乱。删掉后列表里全是中文台。
//
//  ---- 想再加回来 / 加自己的台 ----
//  方式一（不重编译，推荐）：长按 BOOT 3 秒进配网页面，文本框里每行一个
//        我的电台|http://1.2.3.4:8000/stream
//    保存后重启即可，这些台会排在原厂台后面。
//
//  方式二（写进固件）：把下面 USE_BUILTIN_STATIONS 改成 1，然后在
//  kBuiltinStations[] 里照格式填，重新编译烧录。
//
//  规则：一行一个台，{ "显示名称", "流地址" }，用逗号结尾。
//  ✔ 支持：MP3 / AAC 的 http/https 直链（含 Icecast/Shoutcast 的 ICY 流）
//  ✘ 不支持：m3u8 (HLS) —— 国内大多数电台 App 直播源都是 HLS，用不了
// ============================================================================
#pragma once

// 0 = 不启用内置台（当前）；1 = 启用下面这张表
#define USE_BUILTIN_STATIONS 0

struct Station {
  const char* name;
  const char* url;
};

#if USE_BUILTIN_STATIONS

static const Station kBuiltinStations[] = {
  // ---- 自己加台，照上面格式往下写 ----
  // { "中国之声",  "http://example.com/live/zgzs.mp3" },
  { "Radio Paradise 美式摇滚",  "https://stream.radioparadise.com/mp3-128" },
};

#define BUILTIN_STATION_COUNT (sizeof(kBuiltinStations) / sizeof(kBuiltinStations[0]))

#else

#define BUILTIN_STATION_COUNT 0

#endif
