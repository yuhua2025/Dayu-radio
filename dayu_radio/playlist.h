// ============================================================================
//  playlist.h —— 运行期电台表（主程序和预取代理共用）
// ============================================================================
#pragma once

#include <Arduino.h>
#include <vector>

struct Entry {
  String name;
  String url;
};

// 定义在 dayu_radio.ino；开机 buildList() 一次性建好，之后只读。
extern std::vector<Entry> g_list;
