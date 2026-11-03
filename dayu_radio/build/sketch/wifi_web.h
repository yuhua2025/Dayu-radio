#line 1 "/Users/dayu/WorkBuddy/2026-11-02-10-50-15/Dayu-radio/dayu_radio/wifi_web.h"
// ============================================================================
//  wifi_web.h —— WiFi 联网 + 网页设置（自实现，整套移植自「小米闹钟」项目）
//
//  结构与闹钟版一致，差异都因收音机的特殊性而来：
//   1. 扫描改异步（WiFi.scanNetworks(true) + 轮询 /scan）—— 收音机重配网时可能
//      还在播歌，闹钟版阻塞 2~4 秒的同步扫描会把主循环卡住（音频断流）。
//   2. 连上 WiFi 之后的"开播"不在这里做，回调 ino 里的 bootstrapAfterWifi()：
//      setup 直连成功走一遍（带开机画面节奏），配网成功后从 loop 里再走一遍。
//   3. WiFi 凭据存收音机自己的 NVS（"radio" 命名空间 wssid/wpass），
//      首次升级时用 WiFi.begin() 无参连接吃旧 WiFiManager 存的凭据，连上即迁移。
//
//  ★ 本文件必须在 playStation/setVolume/buildList/g_list 等定义之后 include
//    （它直接调这些收音机函数，和闹钟项目 include web_pages.h 的做法同理）。
// ============================================================================
#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "web_pages.h"
#include "spectrum.h"
#include "battery.h"

WebServer server(80);
DNSServer dnsServer;

char staSSID[33] = "";
char staPass[65] = "";

bool apActive   = false;
bool connecting = false;
uint32_t connectStartMs = 0;
uint32_t apCloseAtMs    = 0;
uint32_t lastRetryMs    = 0;
int  lastFailCode = 0;

// false = WiFi 还没就绪、收尾流程（代理/NTP/开播）还没跑过。
// netTask 检测到配网成功后靠它决定要不要触发 bootstrapAfterWifi。
bool g_bootstrapped = false;

// 配网成功后由 netTask 调它，完成 proxy 启动 / NTP / 开播（定义在 dayu_radio.ino）
void bootstrapAfterWifi(bool visual);

// ---------------------------------------------------------------- 凭据存取
static void loadWifiCreds() {
  String s = prefs.getString("wssid", "");
  String p = prefs.getString("wpass", "");
  strlcpy(staSSID, s.c_str(), sizeof(staSSID));
  strlcpy(staPass, p.c_str(), sizeof(staPass));
}

static void saveWifiCreds(const char* ssid, const char* pass) {
  strlcpy(staSSID, ssid ? ssid : "", sizeof(staSSID));
  strlcpy(staPass, pass ? pass : "", sizeof(staPass));
  prefs.putString("wssid", staSSID);
  prefs.putString("wpass", staPass);
}

static void clearWifiCreds() {
  prefs.remove("wssid");
  prefs.remove("wpass");
  // ★ 同时把"已迁移旧凭据"标志置位：否则重启后 staSSID 为空，会走
  //   WiFi.begin() 无参分支，把系统 NVS 里 WiFiManager 留下的旧凭据**又迁回来** ——
  //   用户长按清配网、重启一看还是连回老网络，看起来就是"恢复原始设置没生效"。
  prefs.putBool("wmigrated", true);
  staSSID[0] = 0;
  staPass[0] = 0;
}

// ---------------------------------------------------------------- 配网热点
static void startPortal() {
  if (apActive) { apCloseAtMs = 0; return; }
  WiFi.mode(WIFI_AP_STA);
  delay(50);
  // 开放热点，信道 6 —— 默认信道 1 在楼道里被十几台路由器挤着，配网页会又慢又断。
  // 第 4 个参数是"最多几个客户端"，配网只服务一台手机，多接只会摊薄带宽。
  WiFi.softAP(AP_NAME, nullptr, 6, 0, 4);
  dnsServer.start(53, "*", WiFi.softAPIP());
  apActive = true;
  apCloseAtMs = 0;
  uiPortal(AP_NAME);
  Serial.printf("[NET] 热点 %s 已开 → http://%s\n",
                AP_NAME, WiFi.softAPIP().toString().c_str());
}

static void stopPortal() {
  if (!apActive) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  apCloseAtMs = 0;
  Serial.println(F("[NET] 热点已关闭"));
  uiTick(true);                            // 画面从"配网提示"回到主界面
}

// ---------------------------------------------------------------- 连接状态机
static void beginConnect(const char* ssid, const char* pass) {
  saveWifiCreds(ssid, pass);
  WiFi.begin(staSSID, staPass);
  connecting     = true;
  connectStartMs = millis();
  lastFailCode   = 0;
  Serial.printf("[NET] 正在连接 \"%s\" ...\n", staSSID);
}

static String failText(int code) {
  switch (code) {
    case WL_NO_SSID_AVAIL:
      return String(F("找不到这个网络。请确认路由器 2.4G 已开启、名称没写错；ESP32 搜不到 5G 频段。"));
    case WL_CONNECT_FAILED:
      return String(F("密码不对，或路由器用了 WPA3 / 企业级认证。请把加密方式改成 WPA2-PSK。"));
    case WL_CONNECTION_LOST:
      return String(F("连接中断，信号太弱。靠近路由器再试。"));
    case WL_DISCONNECTED:
      return String(F("连不上，请检查密码、路由器 MAC 白名单或是否开了防蹭网。"));
    default:
      return String(F("连接失败（错误码 ")) + String(code) + String(F("），请重试。"));
  }
}

// 连接轮询：netTask() 每圈都调。与闹钟版一致，20 秒判失败。
static void netTask() {
  uint32_t now = millis();

  if (connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      connecting   = false;
      lastFailCode = 0;
      g_wifiOK     = true;
      Serial.printf("[NET] 已连接 %s  IP %s\n", staSSID, WiFi.localIP().toString().c_str());
      if (apActive) apCloseAtMs = now + AP_AFTER_CONNECT_MS;  // 留一分钟让手机看完结果
      if (!g_bootstrapped) bootstrapAfterWifi(false);         // 配网完成 → 开播
      return;
    }
    if (now - connectStartMs > 20000) {           // 20 秒还没连上就算失败
      connecting   = false;
      lastFailCode = (int)WiFi.status();
      Serial.printf("[NET] 连接失败 code=%d\n", lastFailCode);
      WiFi.disconnect(true, false);
      startPortal();                              // 重开热点，页面显示失败原因
    }
    return;
  }

  // 连上之后延时关热点
  if (apActive && apCloseAtMs && now >= apCloseAtMs &&
      WiFi.status() == WL_CONNECTED) {
    stopPortal();
  }

  // 掉线兜底重连（热点开着的时候不抢，交给用户在配置页操作）
  if (!apActive && staSSID[0] && WiFi.status() != WL_CONNECTED &&
      now - lastRetryMs > 30000) {
    lastRetryMs = now;
    Serial.println(F("[NET] 掉线重试"));
    WiFi.reconnect();
    connecting     = true;
    connectStartMs = now;
    lastFailCode   = 0;
  }
}

// setup 里用的阻塞等待：连 WiFi 最多等 timeoutMs，期间推进开机画面。
static bool netWait(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > timeoutMs) return false;
    uiBootStep("正在连接 WiFi", 560 + (int)((millis() - t0) * 300 / timeoutMs));
    delay(120);
  }
  return true;
}

// ---------------------------------------------------------------- HTTP 工具
static String jsonEscape(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

static String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

// 这个请求是不是从本机热点（192.168.4.x）进来的？
// 手机连着热点时，socket 的"本地地址"就是热点自己的 IP；走路由器时是 STA 的 IP。
// 点"重新配网"时设备仍然连着路由器，两种访问会同时存在，必须靠这个区分。
static bool reqFromAp() {
  if (!apActive) return false;
  WiFiClient c = server.client();
  IPAddress li = c.localIP();
  if ((uint32_t)li != 0) return li == WiFi.softAPIP();
  // 个别情况下拿不到 localIP，退回按网段判断
  IPAddress ri = c.remoteIP();
  return ((uint32_t)ri & 0xFFFFFF00UL) == ((uint32_t)WiFi.softAPIP() & 0xFFFFFF00UL);
}

// ---------------------------------------------------------------- 路由
// 大页面分块发送（1KB 一块）：浏览器不用等整个 8~9KB 到齐就能开始解析首屏，
// 手机上"白屏等待"明显缩短。WebServer 原生 send(200,type,bigString) 是整包排队写，
// 慢速 AP 下手机端经常要干等好几秒。
static void sendHtmlChunked(const char* html, size_t len) {
  server.setContentLength(len);
  server.send(200, "text/html; charset=utf-8", "");
  const size_t CH = 1024;
  for (size_t off = 0; off < len; off += CH) {
    size_t n = (len - off > CH) ? CH : (len - off);
    server.sendContent(html + off, n);
  }
}

static void handleRoot() {
  bool viaAp = reqFromAp();

  // 还没连上网，或者手机是连着本机热点进来的 → 首页给配网页
  if (WiFi.status() != WL_CONNECTED || viaAp) {
    sendHtmlChunked(WIFI_HTML, sizeof(WIFI_HTML) - 1);
    return;
  }

  String page = PAGE_HTML;

  // 选台下框：服务端生成（收音机的台可能上百个，前端不好从哪拿全列表）
  String opts;
  for (size_t i = 0; i < g_list.size(); ++i) {
    char b[180];
    snprintf(b, sizeof(b), "<option value=\"%u\"%s>%u · %s</option>",
             (unsigned)i, (i == g_idx) ? " selected" : "",
             (unsigned)(i + 1), htmlEscape(g_list[i].name).c_str());
    opts += b;
  }
  page.replace("{{OPTIONS}}", opts);
  page.replace("{{IDX}}",     String((unsigned)g_idx));
  page.replace("{{TOTAL}}",   String((unsigned)g_list.size()));
  page.replace("{{VOLMAX}}",  String((unsigned)VOLUME_STEPS));
  page.replace("{{VOL}}",     String((unsigned)g_vol));

  // 频谱样式下拉
  String so;
  int curSpec = spectrumGetStyle();
  for (int i = 0; i < SPEC_COUNT; i++) {
    char sb[160];
    snprintf(sb, sizeof(sb), "<option value=\"%d\"%s>%s</option>",
             i, (i == curSpec) ? " selected" : "", spectrumStyleName(i));
    so += sb;
  }
  page.replace("{{SPECOPTS}}", so);

  page.replace("{{CUSTOM}}",  htmlEscape(prefs.getString("custom", "")));
  page.replace("{{WIFI}}",    htmlEscape(String(staSSID)));
  page.replace("{{RSSI}}",    String(WiFi.RSSI()));
  page.replace("{{IP}}",      WiFi.localIP().toString());
  page.replace("{{FW}}",      FW_VERSION);

  sendHtmlChunked(page.c_str(), page.length());   // 同样分块，别整包排队
}

// ★ 异步扫描：绝不阻塞主循环。第一次调用触发后台扫描并回 pending，
//   前端每 400ms 再来取一次，扫完这趟才回真实结果。
static void handleScan() {
  int st = WiFi.scanComplete();
  if (st == WIFI_SCAN_RUNNING || st == WIFI_SCAN_FAILED) {
    if (st == WIFI_SCAN_FAILED) WiFi.scanNetworks(true);   // true = 异步
    server.send(200, "application/json; charset=utf-8", "{\"pending\":1}");
    return;
  }
  String j = "{\"n\":[";
  int cnt = 0;
  for (int i = 0; i < st && cnt < 15; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    int rssi = WiFi.RSSI(i);
    int bars = (rssi >= -55) ? 4 : (rssi >= -67) ? 3 : (rssi >= -75) ? 2 : 1;
    if (cnt) j += ",";
    j += "{\"s\":\"" + jsonEscape(s) + "\",\"bars\":" + String(bars) +
         ",\"e\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
    cnt++;
  }
  j += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json; charset=utf-8", j);
}

static void handleConnect() {
  String s = server.arg("ssid");
  String p = server.arg("pass");
  if (s.length() == 0) { server.send(200, "text/plain", "no-ssid"); return; }
  beginConnect(s.c_str(), p.c_str());
  server.send(200, "text/plain", "ok");
}

static void handleNetStatus() {
  String j = "{";
  if (WiFi.status() == WL_CONNECTED) {
    j += "\"st\":2,\"ip\":\"" + WiFi.localIP().toString() +
         "\",\"ssid\":\"" + jsonEscape(String(staSSID)) + "\",\"msg\":\"\"";
  } else if (connecting) {
    j += "\"st\":1,\"ip\":\"\",\"ssid\":\"\",\"msg\":\"\"";
  } else if (lastFailCode) {
    j += "\"st\":3,\"ip\":\"\",\"ssid\":\"\",\"msg\":\"" + jsonEscape(failText(lastFailCode)) + "\"";
  } else {
    j += "\"st\":0,\"ip\":\"\",\"ssid\":\"\",\"msg\":\"\"";
  }
  j += "}";
  server.send(200, "application/json; charset=utf-8", j);
}

static void handlePortal() {
  startPortal();
  // 手动开的配网热点：没人操作的话 10 分钟后自动关，不要一直开着
  apCloseAtMs = millis() + AP_IDLE_CLOSE_MS;

  String html = F("<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>配网页已开启</title></head>"
                  "<body style=\"font:16px/1.75 -apple-system,'PingFang SC',sans-serif;"
                  "padding:24px;background:#f2f2f7;color:#1c1c1e\">"
                  "<h3 style='margin:0 0 10px'>配网热点已开启</h3>"
                  "<p>1. 打开手机的 WiFi 列表，连上 <b>" AP_NAME "</b>（无密码）<br>"
                  "2. 连上后浏览器打开 <b>http://192.168.4.1</b>，通常会自动弹出配网页<br>"
                  "3. 选家里的 WiFi、输密码，点“连接”</p>"
                  "<p style='color:#8e8e93;font-size:14px'>连上新 WiFi 后热点会自动关闭，"
                  "再把手机切回家里的 WiFi 就能进设置页。</p>"
                  "</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

// ---------------------------------------------------------------- 收音机 API

// 网页换台请求（-1 = 无）。换台要跑预热+连接全程（零点几到几秒），绝不能在
// HTTP handler 里同步做 —— 那会让浏览器转圈等到超时。先记下台号立即回 ok，
// 由 webLoop() 在主循环里执行，体验和按实体键完全一致。
static volatile int16_t g_webTune = -1;

// 播放状态轮询：/api/status
static void handleStatus() {
  String j = "{\"idx\":" + String((unsigned)g_idx) +
             ",\"total\":" + String((unsigned)g_list.size()) +
             ",\"playing\":" + String(g_heardSound ? 1 : 0) +
             ",\"vol\":" + String((unsigned)g_vol) +
             ",\"rssi\":" + String(WiFi.RSSI()) +
             ",\"name\":\"" + jsonEscape(g_list.empty() ? String("--")
                                        : g_list[g_idx].name) + "\"" +
             ",\"title\":\"" + jsonEscape(String(g_title)) + "\"" +
             ",\"status\":\"" + jsonEscape(String(g_heardSound ? "播放中" : "连接中")) + "\"" +
             ",\"spec\":" + String(spectrumGetStyle()) +
             ",\"batt\":" + String(batteryPercent()) +
             ",\"chg\":" + String(batteryCharging() ? 1 : 0) +
             ",\"mv\":" + String((unsigned)batteryMilliVolts()) +
             ",\"ip\":\"" + WiFi.localIP().toString() + "\"" +
             ",\"heap\":" + String((unsigned)ESP.getFreeHeap()) +
             ",\"psram\":" + String((unsigned)ESP.getFreePsram()) +
             ",\"fw\":\"" FW_VERSION "\"" +
             "}";
  server.send(200, "application/json; charset=utf-8", j);
}

static void handleVol() {
  int v = server.arg("v").toInt();
  setVolume(v);
  server.send(200, "text/plain", "ok");
}

// 频谱样式切换：立即生效 + 落 NVS（key "spec"），不用重启
static void handleSpec() {
  if (server.hasArg("s")) {
    int v = server.arg("s").toInt();
    if (v >= 0 && v < SPEC_COUNT) {
      spectrumSetStyle(v);
      prefs.putUChar("spec", (uint8_t)v);
      Serial.printf("[HTTP] 频谱样式 → %d %s\n", v, spectrumStyleName(v));
      server.send(200, "text/plain", "ok");
      return;
    }
  }
  server.send(200, "text/plain", "bad-style");
}

static void handleTune() {
  int i = server.arg("i").toInt();
  if (g_list.empty() || i < 0 || i >= (int)g_list.size()) {
    server.send(200, "text/plain", "bad-index");
    return;
  }
  g_webTune = (int16_t)i;
  server.send(200, "text/plain", "ok");
}

static void handleTuneNext() {
  if (!g_list.empty()) g_webTune = (int16_t)stepAlive(g_idx, 1);
  server.send(200, "text/plain", "ok");
}

static void handleTunePrev() {
  if (!g_list.empty()) g_webTune = (int16_t)((g_idx + g_list.size() - 1) % g_list.size());
  server.send(200, "text/plain", "ok");
}

// 保存电台表：POST d="名称|URL\n..."（网页 textarea 原文，竖线分隔，逗号兼容）
// 不重启：重建列表 + 换到当前台。台号越界就回 0。
// buildList 很快（纯内存）在这里做；playStation 异步（见 g_webTune）。
static void handleStationSave() {
  String d = server.arg("d");
  if (d.length() > 4000) d = d.substring(0, 4000);   // 和 WiFiManager 时代的上限一致
  prefs.putString("custom", d);
  buildList(d);
  if (g_idx >= g_list.size()) g_idx = 0;
  g_idxDirty = true;
  g_webTune = (int16_t)g_idx;
  Serial.printf("[HTTP] 电台表已更新，共 %u 个台，重播 %u 号台\n",
                (unsigned)g_list.size(), (unsigned)g_idx);
  server.send(200, "text/plain", "ok");
}

static void handleRestart() {
  server.send(200, "text/plain", "ok");
  delay(200);
  ESP.restart();
}

static void handleResetWifi() {
  server.send(200, "text/plain", "ok");
  delay(200);
  clearWifiCreds();
  delay(300);
  ESP.restart();
}

// 任意未知路径：从热点进来的请求跳到配网页（触发手机弹「登录网络」）；
// 走路由器的请求保持 404，别乱跳到一个它根本连不上的 192.168.4.1
static void handleNotFound() {
  if (reqFromAp()) {
    String url = String("http://") + WiFi.softAPIP().toString() + "/";
    server.sendHeader("Location", url);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "404");
  }
}

// ---------------------------------------------------------------- 对外接口

// 注册路由 + 启动服务。必须在 WiFi 初始化之后调（WebServer 依赖 lwIP）。
static void webSetup() {
  server.on("/",           handleRoot);
  server.on("/scan",       handleScan);
  server.on("/connect",    handleConnect);
  server.on("/netstatus",  handleNetStatus);
  server.on("/portal",     handlePortal);
  server.on("/api/status", handleStatus);
  server.on("/vol",        handleVol);
  server.on("/spec",       handleSpec);
  server.on("/tune",       handleTune);
  server.on("/tunenext",   handleTuneNext);
  server.on("/tuneprev",   handleTunePrev);
  server.on("/stationsave", handleStationSave);
  server.on("/restart",    handleRestart);
  server.on("/resetwifi",  handleResetWifi);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[NET] 网页服务已启动（80 端口）");
}

// 主循环每圈都调：DNS 捕获门户 + HTTP + 连接状态机。
// 空闲时 handleClient 只有几微秒开销；处理请求的几~几十毫秒由 lwIP 缓冲兜着，
// 换台抢跑窗口里少处理一圈也不会丢请求。
static void webLoop() {
  if (apActive) dnsServer.processNextRequest();
  server.handleClient();
  netTask();

  // 网页发来的换台请求：在这里落地，和按实体键走完全相同的 playStation 路径
  if (g_webTune >= 0) {
    int16_t i = g_webTune;
    g_webTune = -1;
    Serial.printf("[HTTP] 网页换台 → %d 号台\n", (int)i);
    playStation((uint16_t)i);
  }
}

// 清配网并重启（BOOT 长按 / 串口 w / 网页 /resetwifi 共用）
static void resetWiFiAndReboot() {
  uiMsg("正在清除配网", "清完自动重启");
  clearWifiCreds();
  delay(600);
  ESP.restart();
}
