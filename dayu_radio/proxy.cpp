// ============================================================================
//  proxy.cpp —— 本机中继代理，实现见 proxy.h 顶部说明
//
//  整个模块跑在自己的 FreeRTOS 任务里（core 0），因为 audio.connecttohost()
//  是阻塞的：如果代理放在 loop() 里，换台时 loop() 正卡在等响应，就没人来
//  accept 这个连接，直接死锁。
//
//  连接关系（同一时刻最多两条上游）：
//     audio ──连──> 127.0.0.1:18080 ──代理──> 正在播的那个台（s_up）
//                                             下一个台（s_pup，边抽边存）
//
//  【为什么缓冲要从 body 第一个字节线性攒起】
//  蜻蜓FM 用 Transfer-Encoding: chunked，缓冲必须从**流的第一个 body 字节**开始
//  才有效（从中间开始会令解码器对不上块边界，直接没声音）。
//  【为什么阈值要分层：头就绪 ≠ 攒够 18KB】
//  以前只有一条判据"攒够 18432 字节才算能服务"，结果是：
//    · 命中预取的台 → 130ms 出声，完美；
//    · 没预取的台（开机第一台 / 一次跳好几个台 / 预取过期）→ warmWait 要从零
//      攒 18KB。64kbps 只有 ~8KB/s，光攒数据就 2.3 秒，正好烧满 3500ms 预算；
//      预算烧完退回直连，又撞进 audio 库的响应头忙等 —— 用户看到的就是"卡死"。
//  所以现在把"能不能服务"拆成两级：
//    · proxyHasHeader()  —— 响应头到手就能连。audio 的头解析是毫秒级，
//                            剩下的 body 它自己边连边拉（和直连时的行为一样）。
//    · proxyCanServe()   —— 头 + body 都有，连上就是接近零等待的"命中"。
//  这样冷台的时间不再"堵"在 warmWait 里，而是摊在 loop() 里 —— 界面能动、
//  秒数在涨、按键能响应、旧台还在响，观感从"死机"变成"在连"。
//
//  攒够 16KB 仍然有价值：上游 TCP 窗口撑满、连接僵住时，光靠这 16KB 也够解码器
//  起步和播放两秒，等 socket 自己重新爬坡。放太久（>45s）会作废重暖，避免拿一条
//  很旧的连接去用。
// ============================================================================
#include "proxy.h"

#include <WiFi.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "playlist.h"

#if USE_PREFETCH_PROXY

// ---------------------------------------------------------------- 参数
#define PROXY_PORT    18080
#define PROXY_TICK_MS 2

// 【18KB 这个门槛是怎么来的】
// ESP32-audioI2S 里 `constexpr size_t m_frameSizeMP3 = 18000`（注释写着
// "more than one icy-metaint"），而 processWebStream 只有在内缓冲
// bufferFilled() > maxFrameSize 时才把 m_f_stream 置真 —— 那一刻才真正出声。
// 注意：这个门槛 audio 自己会跨过，**不需要我们替它等**。我们替它等的唯一好处
// 是"连上就出声"，代价是冷台要白等 2.3 秒 —— 见下面 PRE_MIN_READY 的注释。
// ★ 起播门槛（2026-11 按用户要求"宁可慢 1~2 秒，也别卡"整体抬高）
//   之前 PRE_TARGET 32KB / 就绪 24KB / 垫 24KB ≈ 3 秒存粮。实测仍有台偶发卡顿：
//   存粮 3 秒，CDN 一个 2~3 秒的供数空窗就把存粮吃光，正好落在看门狗 3 秒线附近。
//   现在统一抬到 5~6 秒：起播多等几百毫秒，换来的是"出声之后一两秒内不会再断"。
#define PRE_TARGET    49152    // 预取多少 body 字节就停手。48KB ≈ 6 秒（64kbps）
#define PRE_MIN_READY 40960    // 攒到这么多 = "命中"级就绪：连上就出声。40KB ≈ 5 秒存粮
#define PRE_HEAD_MAX  1400     // 响应头上限
#define PRE_STALE_MS  45000    // 预取放太久就作废重暖
#define PRE_TAKEOVER_GRACE_MS  1500   // 就绪槽被跳过时，给 audio 接管留的宽限（见 case 3 注释）
#define UP_CONN_MS    3000     // attachUpstream 现连的上游 TCP 超时（冷路径，可以宽松）
#define PRE_CONN_MS   2500     // 预取那条连接的超时。实测大量蜻蜓 CDN 边缘首连恰好卡在
                               // 1500~2000ms（重试却 15ms 就通），1500ms 会把好台连败 2 次
                               // 冤判成死台，用户按过去"一跳跳过 6 个台"。2500ms 下
                               // 真死台的判死代价是 2×2.5s+300ms ≈ 5.3s，可接受。
#define PRE_RETRY_MS   300     // 预取连不上多久后重试
#define PRE_MAX_FAILS    2     // 连续失败这么多次就告诉上层"这个台连不上"
#define DUMP_CHUNK    2048     // 每次往 audio 那侧最多写这么多，避免写阻塞
#define UP_STALL_MS   6000     // 转发中上游连续这么久一个字节都没有 = CDN 断流，掐断两侧。
                               // ★ 从 3000 放宽到 6000：起播存粮提到 5~6 秒后，3 秒就掐
                               //   等于"手里还有 3 秒歌，却主动断了去重连" —— 那是我们自己
                               //   制造的卡顿。存粮能扛 5 秒，就给上游 6 秒机会。
                               //   不掐的代价也有限：audio 读超时现在是 1500ms，
                               //   主循环不会像以前那样冻十几秒。
                               //   掐断后 audio 收到 EOF → g_eof → 上层自动重连。
#define CUSH_TARGET   40960    // 实时转发路径的起播缓冲垫：攒够这么多才开始喂 audio。
                               // "body 还没到"就接管的台没有预取存粮，直接边收边放的话
                               // audio 的输出门槛（18KB）被 CDN 一个突发填满就开始出声，
                               // 突发一停立刻断粮 —— 就是"有几个台卡顿一会儿才正常"。
                               // 40KB ≈ 5 秒存粮，和预取路径一个标准。
#define CUSH_MAX_MS   1200     // 攒垫最多等这么久。2000 太长了 —— 整机自检里冷路径
                               // 是「预热 1.2s + 连接 2.5s + 攒垫 2s」三段串行，
                               // 叠起来就七八秒。1.2 秒到点有多少喂多少，先出声再说。

// ---------------------------------------------------------------- 状态
static WiFiServer  s_srv(PROXY_PORT);
static bool        s_ok = false;

// ---- audio 那一侧 ----
static WiFiClient  s_cl;
static uint8_t     s_sstate = 0;      // 0 空闲 1 收请求行 2 已解析 3 转发中 4 吐预取缓冲
static String      s_sreq;
static uint32_t    s_st0 = 0;
static size_t      s_dumpOff = 0;     // 吐缓冲的进度
static size_t      s_dumpLen = 0;
static uint32_t    s_upDataAt = 0;    // 上游最近一次来数据的时刻（断流看门狗用）

// ---- 实时转发路径的起播缓冲垫（PSRAM，见 CUSH_TARGET 注释）----
static uint8_t*    s_cush = nullptr;  // 垫缓冲
static uint32_t    s_cushN = 0;       // 已攒字节数
static uint32_t    s_cushOff = 0;     // 已吐字节数（攒完进入吐出阶段后 > 0 或 s_cushFlush）
static bool        s_cushFlush = false; // true = 攒够了/超时了，正在往 audio 吐
static bool        s_cushActive = false;// true = 这条转发流要先攒垫
static uint32_t    s_cushT0 = 0;      // 攒垫开始时刻

static void cushReset() {
  s_cushN = 0; s_cushOff = 0; s_cushFlush = false; s_cushActive = false; s_cushT0 = 0;
}
static bool        s_dumping = false; // 吐缓冲期间 s_pbuf 归 audio 用，预取不许动

// ---- 上游 ----
static WiFiClient  s_up;              // 现连的上游
static WiFiClient  s_pup;             // 预取的上游
static WiFiClient* s_upP  = nullptr;  // 当前正在泵的上游
static uint16_t    s_servingIdx = 0xFFFF;

// ---- 预取 ----
static char        s_hbuf[PRE_HEAD_MAX];
static size_t      s_hlen = 0;
static uint8_t*    s_pbuf = nullptr;  // 线性缓冲：从 body 第一个字节开始存
static size_t      s_pn   = 0;        // 已存字节
static uint16_t    s_pidx = 0xFFFF;   // 正在暖的台
static uint16_t    s_want = 0xFFFF;   // 想暖的台
static uint16_t    s_cur  = 0xFFFF;   // 刚刚交给 audio 的那个台（proxySetNext 的第一个参数）
static uint8_t     s_pstate = 0;      // 0 待连 1 等头 2 收 body 3 就绪
static uint32_t    s_pt0 = 0;
static uint32_t    s_pdoneAt = 0;
static uint32_t    s_pretryAt = 0;
static uint8_t     s_pfails = 0;      // 连续连接失败次数（上层据此判死台）

// ---------------------------------------------------------------- 小工具
static bool splitUrl(const String& u, String& host, uint16_t& port, String& path) {
  if (!u.startsWith("http://")) return false;
  const int s = 7;
  int e = u.indexOf('/', s);
  String hp = (e < 0) ? u.substring(s) : u.substring(s, e);
  path = (e < 0) ? String("/") : u.substring(e);
  port = 80;
  int c = hp.indexOf(':');
  if (c > 0) { port = (uint16_t)hp.substring(c + 1).toInt(); host = hp.substring(0, c); }
  else       { host = hp; }
  host.trim();
  return host.length() > 0;
}

// 和 ESP32-audioI2S 内部发的请求保持完全一致（Audio.cpp:870 那段），
// 这样 icy-metaint / 歌曲名这些行为不会因为走代理而变化。
static String buildReq(const String& host, const String& path) {
  String r;
  r.reserve(280);
  r += "GET ";            r += path;  r += " HTTP/1.1\r\n";
  r += "Host: ";          r += host;  r += "\r\n";
  r += "Icy-MetaData:1\r\n";
  r += "Accept:*/*\r\n";
  r += "User-Agent: VLC/3.0.21 LibVLC/3.0.21 AppleWebKit/537.36 (KHTML, like Gecko)\r\n";
  r += "Accept-Encoding: identity;q=1,*;q=0\r\n";
  r += "Connection: keep-alive\r\n\r\n";
  return r;
}

static void preReset(const char* why) {
  // 预取那条 socket 可能已经被"命中"接管去转发了，这时绝不能关
  (void)why;
  if (s_upP != &s_pup) s_pup.stop();
  s_hlen  = 0;
  s_pn    = 0;
  s_pidx  = 0xFFFF;
  s_pstate = 0;
  s_pdoneAt = 0;
  s_pfails  = 0;
}

// ---------------------------------------------------------------- 预取状态机
// 全程非阻塞（除了 connect 那一下，热路径上只有 20~50ms，DNS 已缓存）。
// 之所以不能阻塞：正在播的台全靠这个任务在转发字节，卡住就会断音。
static void preStep() {
  // 正在把预取缓冲喂给 audio：s_pbuf 还被占用着，谁也别动
  if (s_dumping) return;

  // 目标变了：**只在填充中**才作废。
  // 已经攒好的（state 3）不能清：connecttohost() 一连上+发完请求就返回，
  // 主循环紧接着会把 want 指向下一个台，可音频那边还没读到这份缓冲呢 ——
  // 一清就正好把要用的东西扔掉，表现为"刚说已就绪，下一秒就没了"。
  //
  // ★ state 2（头已完整、body 还在攒）同样不能清，判据必须带上 s_pidx == s_cur。
  //   这就是"命中判成没命中"的根源：warmWait 等到头 → 回来 connect → 连上之后
  //   第一件事就是 proxySetNext(本台, 下一台)，want 一变，这份刚到手的预取立刻
  //   被 preReset 清掉；等 audio 真连过来时槽里已经空了，只好现连上游，
  //   于是 audio 的 parseHttpResponseHeader 在 loop() 里忙等 1.8 秒 —— 整机停摆。
  //   日志长这样：`preReset(want变了) 清掉 38 号台` 紧跟 `38 号台 没预取到`。
  if ((s_pstate == 1 || s_pstate == 2) && s_want != s_pidx && s_pidx != s_cur) {
    preReset("want变了");
    return;
  }

  switch (s_pstate) {

    case 0: {   // 待连
      if (s_want == 0xFFFF || s_want >= g_list.size()) return;
      if (s_upP == &s_pup) return;                 // 预取那条正在转发，别抢

      uint32_t now = millis();
      if ((int32_t)(now - s_pretryAt) < 0) return;

      String host, path; uint16_t port = 0;
      if (!splitUrl(g_list[s_want].url, host, port, path)) { s_pretryAt = now + 3000; return; }

      s_pup.stop();
      uint32_t tConn = millis();
      if (!s_pup.connect(host.c_str(), port, PRE_CONN_MS)) {
        s_pup.stop();
        s_pretryAt = now + PRE_RETRY_MS;
        if (s_pfails < 255) ++s_pfails;
        Serial.printf("[proxy] %u 号台 连接失败（%ums，第 %u 次）\n",
                      (unsigned)s_want, (unsigned)(millis() - tConn), (unsigned)s_pfails);
        if (s_pfails == PRE_MAX_FAILS)
          Serial.printf("[proxy] %u 号台 连续 %u 次连不上，跳过\n",
                        (unsigned)s_want, (unsigned)s_pfails);
        return;
      }
      s_pfails = 0;
      s_pup.setNoDelay(true);
      s_pup.print(buildReq(host, path).c_str());
      s_hlen  = 0;
      s_pn    = 0;
      s_pidx  = s_want;
      s_pt0   = millis();
      s_pstate = 1;
      break;
    }

    case 1: {   // 等响应头（服务器会在这里停 150~1650ms，所以必须非阻塞轮询）
      int avail = s_pup.available();
      while (avail-- > 0) {
        int ch = s_pup.read();
        if (ch < 0) break;
        if (s_hlen < PRE_HEAD_MAX) s_hbuf[s_hlen++] = (char)ch;
        if (s_hlen >= 4 && s_hbuf[s_hlen - 4] == '\r' && s_hbuf[s_hlen - 3] == '\n' &&
            s_hbuf[s_hlen - 2] == '\r' && s_hbuf[s_hlen - 1] == '\n') {
          s_pstate = 2;
          break;
        }
      }
      if (s_hlen >= PRE_HEAD_MAX) { preReset("头过长"); }
      else if (millis() - s_pt0 > 5000) { preReset("等头超时"); }
      break;
    }

    case 2: {   // 收 body，攒满 PRE_TARGET 就停手（别再读了，免得把缓冲撑爆）
      int avail = s_pup.available();
      while (avail > 0 && s_pn < PRE_TARGET) {
        int want = (avail > (int)(PRE_TARGET - s_pn)) ? (int)(PRE_TARGET - s_pn) : avail;
        int n = s_pup.read(s_pbuf + s_pn, want);
        if (n <= 0) break;
        s_pn += n;
        avail -= n;
      }
      if (s_pn >= PRE_TARGET) {
        s_pstate  = 3;
        s_pdoneAt = millis();
      } else if (!s_pup.connected() && s_pup.available() == 0) {
        Serial.printf("[proxy] %u 号台 上游断开，放弃预取\n", (unsigned)s_pidx);
        preReset("上游断开");
      }
      break;
    }

    case 3: {   // 就绪，晾着等 audio 来拿；放太久就作废重暖
      // ★ 已就绪的槽如果既不是"刚交给 audio 的那个台"，也不是"下一个要暖的台"，
      //   说明这一跳直接跳过了它（合并跳转 +N 时必然发生）。此时必须立刻作废，
      //   否则它会一直占着唯一的槽位到 45 秒过期，期间再也暖不了新台 ——
      //   表现就是"连按几下之后，后面每一次换台都是冷连接"。
      //
      // ★ 但不能"立刻"：connecttohost() 现在是不等响应头就返回的（头解析推迟到
      //   Audio::loop 的 HTTP_RESPONSE_HEADER 分支），主循环一连上就把 want 指向
      //   下一个台。而代理任务这边"收到 audio 的 TCP 连接"和"解析出请求里的台号"
      //   是两个节拍，中间恰好插进一次本判断的话，就会把 audio 马上要接管的那份
      //   预取当成"被跳过的台"清掉 —— 日志指纹：`preReset(跳过它了) 清掉 N 号台`
      //   紧跟 `N 号台 没预取到，现连`，出声凭空多 1.5 秒。所以刚就绪的槽先宽限
      //   1.5 秒再清：接管几十毫秒内就到，稳赢；真正的 +N 跳转最多晚 1.5 秒重暖。
      if (s_pidx != s_cur && s_pidx != s_want) {
        if (s_pdoneAt && millis() - s_pdoneAt < PRE_TAKEOVER_GRACE_MS) break;
        preReset("跳过它了");
        break;
      }
      if (s_pdoneAt && millis() - s_pdoneAt > PRE_STALE_MS) {
        preReset("放太久");
      }
      break;
    }
  }
}

// ---------------------------------------------------------------- 服务 audio
// 兜底：预取没赶上就自己现连（= 加代理之前的耗时，不会更慢）。
// 只在"没预取到"这条冷路径上跑，此时没有正在转发的流，阻塞几百毫秒无所谓。
static void attachUpstream(uint16_t idx) {
  String host, path; uint16_t port = 0;
  if (!splitUrl(g_list[idx].url, host, port, path)) { s_cl.stop(); s_sstate = 0; return; }

  s_up.stop();
  if (!s_up.connect(host.c_str(), port, UP_CONN_MS)) {
    Serial.printf("[proxy] %u 号台 现连失败\n", (unsigned)idx);
    s_cl.stop();
    s_sstate = 0;
    return;
  }
  s_up.setNoDelay(true);
  s_up.print(buildReq(host, path).c_str());
  s_upP        = &s_up;
  s_servingIdx = idx;
  s_upDataAt   = millis();                 // 断流看门狗从现连这一刻起算
  s_sstate     = 3;
}

static void serveStep() {
  // ---- 有新的 audio 连进来？----
  WiFiClient nc = s_srv.available();
  if (nc) {
    // 连着来的新连接会顶掉旧的（换台/重连时正常发生）
    if (s_upP) { s_upP->stop(); s_upP = nullptr; }
    s_cl.stop();
    cushReset();                           // 起播垫跟着旧流一起作废
    if (s_dumping) {                       // 上一条还没吐完就来了新的，直接放弃缓冲区
      s_dumping = false;
      s_dumpLen = 0;
      s_pn      = 0;
      s_pidx    = 0xFFFF;
      s_pstate  = 0;
    }
    s_cl = nc;
    s_cl.setNoDelay(true);
    s_cl.setTimeout(200);          // 对端不读时写操作最多卡 200ms，别把任务挂死
    s_sreq  = "";
    s_st0   = millis();
    s_sstate = 1;
    s_servingIdx = 0xFFFF;
    return;
  }

  if (s_sstate == 1) {   // 收请求行
    while (s_cl.available()) {
      char ch = (char)s_cl.read();
      if (ch == '\n') { s_sstate = 2; break; }
      if (s_sreq.length() < 220) s_sreq += ch;
    }
    if (s_sstate != 2) {
      if (millis() - s_st0 > 1500) { s_cl.stop(); s_sstate = 0; }
      return;
    }
  }

  if (s_sstate == 2) {   // 解析 "GET /<idx> HTTP/1.1"
    int sp = s_sreq.indexOf(' ');
    String target = (sp > 0) ? s_sreq.substring(sp + 1) : String("");
    int sp2 = target.indexOf(' ');
    if (sp2 > 0) target = target.substring(0, sp2);

    uint16_t idx = 0xFFFF;
    if (target.length() > 1 && target[0] == '/') {
      long v = target.substring(1).toInt();
      if (v >= 0 && v < (long)g_list.size()) idx = (uint16_t)v;
    }
    if (idx == 0xFFFF) {
      Serial.printf("[proxy] 请求看不懂：%s\n", s_sreq.c_str());
      s_cl.print("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
      s_cl.stop();
      s_sstate = 0;
      return;
    }

    // ---- 接管预取连接：响应头必须是**完整的**，body 攒到哪就吐到哪 ----
    // ★ 为什么非要完整的头：ESP32-audioI2S 的 parseHttpResponseHeader() 是纯忙等
    //   （Audio.cpp:4766，`while(true)` 里只读不 delay，超时 5000ms），而且它不只
    //   在 connecttohost 里调 —— `Audio::loop()` 的 HTTP_RESPONSE_HEADER 分支也调。
    //   所以只要 audio 连上来时头还没齐，那个忙等就会落在主循环里，1~2 秒不动。
    //   宁可让 warmWait 多等几百毫秒（旧台还在响），也不能把半截头递过去。
    if (s_pidx == idx && s_pstate >= 2 && s_pstate <= 3) {
      // ★ 把预取那条 socket 的**所有权**交给 s_up，而不是借指针用。
      //   借指针的话 s_upP 会指向 s_pup，预取槽位被自己的转发占住，
      //   后面再也暖不了下一个台。NetworkClient 的 clientSocketHandle 是
      //   shared_ptr，赋值共享句柄，随后 s_pup 重新赋成空对象即完成转移。
      s_up = s_pup;
      s_pup = WiFiClient();
      s_upP = &s_up;

      size_t bodyLen  = s_pn;
      size_t headLen  = s_hlen;

      s_cl.write((const uint8_t*)s_hbuf, headLen);
      s_hlen       = 0;
      s_servingIdx = idx;
      s_upDataAt   = millis();               // 断流看门狗从接管这一刻起算

      // 预取槽位就此交出去，preStep 可以开始暖下一个台
      s_pidx    = 0xFFFF;
      s_pstate  = 0;
      s_pdoneAt = 0;
      s_pfails  = 0;

      if (bodyLen > 0) {
        s_dumpOff = 0;
        s_dumpLen = bodyLen;
        s_dumping = true;
        s_sstate  = 4;
      } else {
        // 只有头、一个 body 字节都还没来：转实时转发，但先攒起播垫（CUSH_TARGET）
        // —— 直接边收边放的话，audio 会被 CDN 的突发节奏带着一卡一卡。
        s_pn     = 0;
        s_sstate = 3;
        if (s_cush) {               // 垫缓冲没分配成功就直接走老路（边收边放）
          s_cushN  = 0;
          s_cushOff = 0;
          s_cushFlush = false;
          s_cushActive = true;
          s_cushT0 = millis();
        }
      }
      return;
    }

    // ---- 没预取到：现连兜底，耗时和加代理之前一样 ----
    attachUpstream(idx);
    return;
  }

  // ---- 吐预取缓冲（非阻塞分批，写完再转成常规转发）----
  if (s_sstate == 4) {
    while (s_dumpOff < s_dumpLen) {
      size_t want = s_dumpLen - s_dumpOff;
      if (want > DUMP_CHUNK) want = DUMP_CHUNK;
      size_t w = s_cl.write(s_pbuf + s_dumpOff, want);
      if (w == 0) break;                    // 对端还没读走，下轮继续
      s_dumpOff += w;
      if (w < want) break;
    }
    if (s_dumpOff >= s_dumpLen) {
      s_dumping = false;
      s_dumpLen = 0;
      s_pn      = 0;
      s_pidx    = 0xFFFF;
      s_pstate  = 0;
      s_pdoneAt = 0;
      s_sstate  = 3;
    }
    return;
  }

  // ---- 转发中 ----
  if (s_sstate == 3 && s_upP) {
    // ---- 起播缓冲垫（实时转发路径）：先攒后喂，起播即稳 ----
    if (s_cushActive) {
      if (!s_cl.connected()) {          // audio 侧没了：垫作废，整条流清理（含攒垫/吐垫两个阶段）
        s_upP->stop(); s_upP = nullptr; s_servingIdx = 0xFFFF; s_sstate = 0; cushReset();
        return;
      }
      if (!s_cushFlush) {
        int avail = s_upP->available();
        if (avail > 0) s_upDataAt = millis();
        while (avail > 0 && s_cushN < CUSH_TARGET) {
          int want = (avail > (int)(CUSH_TARGET - s_cushN)) ? (int)(CUSH_TARGET - s_cushN) : avail;
          int n = s_upP->read(s_cush + s_cushN, want);
          if (n <= 0) break;
          s_cushN += n;
          avail -= n;
        }
        if (s_cushN >= CUSH_TARGET || millis() - s_cushT0 > CUSH_MAX_MS) {
          s_cushFlush = true;   // 垫好了（或超时到点，有多少喂多少）
          s_cushOff = 0;
        } else {
          return;               // 还在攒
        }
      }
      // 吐垫阶段：把攒好的数据分块交给 audio，吐完转常规实时转发
      while (s_cushOff < s_cushN) {
        size_t want = s_cushN - s_cushOff;
        if (want > DUMP_CHUNK) want = DUMP_CHUNK;
        size_t w = s_cl.write(s_cush + s_cushOff, want);
        if (w == 0) return;                 // 对端还没读走，下一轮继续
        s_cushOff += w;
        if (w < want) return;
      }
      cushReset();
      return;   // 本轮到此，下一轮进入常规转发
    }

    int avail = s_upP->available();
    if (avail > 0) {
      s_upDataAt = millis();
      uint8_t tmp[1024];
      int budget = 6144;                      // 每轮最多搬 6KB，别把任务占死
      while (avail > 0 && budget > 0) {
        int want = (avail > (int)sizeof(tmp)) ? (int)sizeof(tmp) : avail;
        int n = s_upP->read(tmp, want);
        if (n <= 0) break;
        size_t w = s_cl.write(tmp, n);        // 对端不读时会短暂阻塞，能接受
        budget -= n;
        avail  -= n;
        if (w != (size_t)n) { s_cl.stop(); break; }
      }
    } else if (!s_upP->connected()) {
      s_cl.stop();                            // 上游结束（掉线 / 服务器踢人）
    } else if (s_upDataAt && millis() - s_upDataAt > UP_STALL_MS) {
      // ★ 上游断流看门狗：CDN 偶尔会"连接还在、一个字节不来"地卡住十几秒。
      //   不掐的话 audio.loop() 在读 socket 时干等（实测 10042ms），主循环整个
      //   冻住 —— 屏幕停在"连接中"、按键没反应，用户看着就像死机。
      //   掐断后 audio 收到 EOF → g_eof → 上层自动重连，1~2 秒内恢复出声。
      Serial.printf("[proxy] %u 号台 上游 %ums 没数据（断流），掐断让上层重连\n",
                    s_servingIdx == 0xFFFF ? 9999u : (unsigned)s_servingIdx,
                    (unsigned)(millis() - s_upDataAt));
      s_cl.stop();
      s_upP->stop();
      s_upP = nullptr;
      s_servingIdx = 0xFFFF;
      s_sstate = 0;
      cushReset();
    }

    if (!s_cl.connected()) {
      if (s_upP) s_upP->stop();
      s_upP = nullptr;
      s_servingIdx = 0xFFFF;
      s_sstate = 0;
      cushReset();
      // ★ 这里千万不能 preReset()：预取槽里装的是"下一个台"的连接，
      //   跟刚断掉的这条流毫无关系。以前在这里清掉，结果每次换台都把
      //   暖好的预取扔掉，表现为"隔一次命中一次"。
    }
  }
}

// ---------------------------------------------------------------- 任务
static void proxyTask(void*) {
  s_srv.begin();
  s_srv.setNoDelay(true);
  Serial.printf("[proxy] 监听 127.0.0.1:%d\n", PROXY_PORT);
  for (;;) {
    serveStep();
    preStep();
    vTaskDelay(pdMS_TO_TICKS(PROXY_TICK_MS));
  }
}

// ---------------------------------------------------------------- 对外接口
bool proxyBegin() {
  if (s_ok) return true;
  if (!s_pbuf) {
    s_pbuf = (uint8_t*)heap_caps_malloc(PRE_TARGET, MALLOC_CAP_SPIRAM);
    if (!s_pbuf) {
      Serial.println("[proxy] PSRAM 分配失败，不用代理");
      return false;
    }
  }
  if (!s_cush) {
    s_cush = (uint8_t*)heap_caps_malloc(CUSH_TARGET, MALLOC_CAP_SPIRAM);
    if (!s_cush) {   // 垫分配失败不致命：实时转发起播可能轻微卡顿，但功能都在
      Serial.println("[proxy] 起播垫 PSRAM 分配失败，实时转发不攒垫");
    }
  }
  BaseType_t r = xTaskCreatePinnedToCore(proxyTask, "proxy", 6144, nullptr, 2, nullptr, 0);
  if (r != pdPASS) {
    Serial.println("[proxy] 任务创建失败，不用代理");
    return false;
  }
  s_ok = true;
  return true;
}

String proxyUrlFor(uint16_t idx) {
  String u = "http://127.0.0.1:";
  u += (unsigned)PROXY_PORT;
  u += '/';
  u += (unsigned)idx;
  return u;
}

void proxySetNext(uint16_t cur, uint16_t next) {
  if (next != s_want) s_pfails = 0;   // 换目标了，失败次数不能算到新台头上
  s_cur  = cur;
  s_want = next;
}

bool proxyReady() { return s_ok; }

// 网络流畅度：距上次从上游读到音频数据的毫秒数。
// 只在"常规转发中"有意义 —— 吐预取存粮/起播垫阶段上游不供数，gap 会虚高，
// 所以那些阶段直接报 0（未知），界面也不显示指示器。
uint16_t proxyFeedGapMs() {
  if (s_sstate != 3 || !s_upP || !s_upDataAt) return 0;
  if (s_dumping || s_cushActive) return 0;
  uint32_t gap = millis() - s_upDataAt;
  return (uint16_t)(gap > 60000 ? 60000 : gap);
}

// "接管即零等待"：头 + 够多的 body 都在手里，连上就出声。
bool proxyCanServe(uint16_t idx) {
  // 正在吐上一条缓冲时不能再接活（s_pbuf 还被占着）
  return s_ok && !s_dumping && s_pstate >= 2 && s_pidx == idx && s_pn >= PRE_MIN_READY;
}

// "头已经在手里"：连下来 audio 的响应头解析是毫秒级，body 边连边拉。
// 比 proxyCanServe 宽松得多 —— 冷台不必等攒够 18KB 才敢连。
bool proxyHasHeader(uint16_t idx) {
  return s_ok && !s_dumping && s_pstate >= 2 && s_pidx == idx && s_hlen > 0;
}

ProxyWarm proxyWarm(uint16_t idx) {
  ProxyWarm w;
  if (!s_ok)                       w.state = -1;
  else if (s_pidx == idx)          w.state = (int8_t)s_pstate;   // 1 等头 / 2 收body / 3 就绪
  else if (s_want == idx)          w.state = 0;                  // 已经点它了，还没连上
  else                             w.state = -1;                 // 代理在忙别的台
  w.bytes = (s_pidx == idx) ? (uint32_t)s_pn : 0;
  // 失败次数只在"确实在试这个台、且还没连上"时才有意义
  w.fails = (s_ok && s_want == idx && s_pstate == 0) ? s_pfails : 0;
  return w;
}

#else  // ---------------- 关掉代理时的空实现 ----------------

bool        proxyBegin()                 { return false; }
bool        proxyCanServe(uint16_t)      { return false; }
bool        proxyHasHeader(uint16_t)     { return false; }
String      proxyUrlFor(uint16_t)        { return String(); }
void        proxySetNext(uint16_t, uint16_t) {}
bool        proxyReady()                 { return false; }
ProxyWarm   proxyWarm(uint16_t)          { return ProxyWarm{-1, 0, 0}; }

#endif
