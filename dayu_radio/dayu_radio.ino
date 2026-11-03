// ============================================================================
//  Dayu-radio —— 网络收音机（源码版）
//  硬件：星智 Cube 1.54 WiFi
//  ESP32-S3 / 16MB flash / 8MB OPI PSRAM / ST7789 240x240 / I2S 喇叭
//
//  功能：
//    · 配网：自实现中文配网页（扫网 + 连接 + 失败原因提示，移植自小米闹钟项目），
//      首次开机起热点 DayuRadio，手机连上后选 WiFi 填密码 → 保存即用
//    · 手机浏览器打开收音机 IP = 网页设置页：选台 / 音量 / 改电台表（即时生效）
//    · BOOT 短按 = 下一个台（按下瞬间就响应），长按 8 秒 = 清空 WiFi 重新配网
//    · 音量键 +/- 调节，掉线自动重连，屏幕显示台名/歌曲名/信号/时间
//
//  编译（arduino-cli）：
//    arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,FlashMode=dio,PSRAM=opi,PartitionScheme=huge_app,CPUFreq=240,USBMode=hwcdc,CDCOnBoot=cdc" --build-path build .
//  烧录：
//    arduino-cli upload  --fqbn "同上" --input-dir build -p /dev/cu.usbmodem101 .
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <vector>

#include "Audio.h"
#include "config.h"
#include "stations.h"
#include "stations-qtfm.h"
#include "playlist.h"
#include "proxy.h"
#include "ui.h"
#include "buttons.h"
#include "spectrum.h"
#include "battery.h"

// ---------------------------------------------------------------- 全局
Audio       audio;
Preferences prefs;

std::vector<Entry> g_list;      // 定义在 playlist.h 里声明，proxy.cpp 也要读

static uint16_t g_idx        = 0;
static uint8_t  g_vol        = VOLUME_DEFAULT;
// ---- 切台音量坡道：把"旧流掐断的爆音 + 新流从直播中间切入的头一秒劣化"藏进音量里 ----
// g_vol 是用户想要的音量；g_appliedVol 是此刻真正喂给 audio 的音量。
// 换台前把 applied 压到 0（淡出），新台出声后每 30ms 往上爬 1 格直到回到 g_vol（淡入）。
// 用户手动动音量键 = 立即接管，坡道作废。
static int8_t   g_appliedVol = VOLUME_DEFAULT;
static bool     g_fadeIn     = false;
static uint32_t g_fadeAt     = 0;
static bool     g_wifiOK     = false;
static bool     g_playing    = false;
static uint32_t g_lastTry    = 0;
static uint32_t g_lastUi     = 0;
static bool     g_volDirty   = false;
static int8_t   g_volDir     = 0;
static uint32_t g_volRepeatAt = 0;
static bool     g_idxDirty   = false;   // 台号变了但还没落盘（见 playStation 里的说明）

static char g_title[160] = {0};   // ICY 里的歌曲名
static char g_icyName[64] = {0};  // 流自报的台名

static volatile bool g_eof        = false;
static volatile bool g_titleDirty = false;

// ---- 换台耗时计时（诊断用，串口里能看到每一段花多久）----
static uint32_t g_tSwitch     = 0;   // 本次换台的起点（按下按键那一刻）
static uint32_t g_tStopped    = 0;   // 旧流已关闭
static uint32_t g_tConnected  = 0;   // TCP 连上、请求已发出
static uint32_t g_lastBar     = 0;   // 上次画进度条的时刻
static uint8_t  g_failTries    = 0;  // 当前这个台连了几次
static uint8_t  g_skipStreak   = 0;  // 连续跳过了几个台都没成功
static bool     g_heardSound  = false;
// ★ "出声"的判据是**真实送进 I2S 的样本数有没有变**（见 spectrum.cpp 的钩子），
//   不是 audio.getBitRate()。这个基准值在每次 connecttohost 之后取，之后只要
//   样本数变了就说明新流真的在出数据 —— 旧台在预热期还在响，用"时刻"比较会误判。
static uint32_t g_sampleBase  = 0;

// ---------------------------------------------------------------- 电台表
static void buildList(const String& custom) {
  g_list.clear();

  // ① 原厂固件内置的蜻蜓FM 台：id 填了才加（id=0 表示还没查到台号，跳过）
  char qtUrl[80];
  int skipped = 0;
  for (size_t i = 0; i < QT_STATION_COUNT && g_list.size() < MAX_STATIONS; ++i) {
    if (!kQtStations[i].id) { ++skipped; continue; }
    snprintf(qtUrl, sizeof(qtUrl), QT_URL_FMT, (unsigned)kQtStations[i].id);
    g_list.push_back({kQtStations[i].name, String(qtUrl)});
  }
  if (skipped) {
    Serial.printf("[list] 原厂台共 %u 个，其中 %d 个台号未填已跳过\n",
                  (unsigned)QT_STATION_COUNT, skipped);
  }

  // ② 内置的公共流（stations.h，当前 USE_BUILTIN_STATIONS 0 已清空）
#if USE_BUILTIN_STATIONS
  for (size_t i = 0; i < BUILTIN_STATION_COUNT && g_list.size() < MAX_STATIONS; ++i) {
    g_list.push_back({kBuiltinStations[i].name, kBuiltinStations[i].url});
  }
#endif

  // ③ 网页设置页填的自定义台，逐行解析
  // 分隔符：竖线 |（最初版就是它）。逗号（英文/中文）作备选兼容。
  int pos = 0;
  while (pos < (int)custom.length() && g_list.size() < MAX_STATIONS) {
    int nl = custom.indexOf('\n', pos);
    if (nl < 0) nl = custom.length();
    String line = custom.substring(pos, nl);
    pos = nl + 1;
    line.trim();
    if (!line.length() || line.startsWith("#")) continue;
    int bar = line.indexOf('|');
    int t   = line.indexOf(',');
    if (bar < 0 || (t >= 0 && t < bar)) bar = t;
    t = line.indexOf("，");
    if (bar < 0 || (t >= 0 && t < bar)) bar = t;
    if (bar <= 0) continue;
    String nm = line.substring(0, bar);
    String uu = line.substring(bar + 1);
    nm.trim();
    uu.trim();
    if (!nm.length() || !uu.startsWith("http")) continue;
    g_list.push_back({nm, uu});
  }
}

// ---------------------------------------------------------------- 音频回调
static void setupAudioCallback() {
  audio.audio_info_callback = [](Audio::msg_t m) {
    switch (m.e) {
      case Audio::evt_streamtitle:
        if (m.s) {
          strlcpy(g_title, m.s, sizeof(g_title));
          g_titleDirty = true;
        }
        break;
      case Audio::evt_name:
        if (m.s) strlcpy(g_icyName, m.s, sizeof(g_icyName));
        break;
      case Audio::evt_eof:
        g_eof = true;
        break;
      case Audio::evt_info:
        // 音频库的各类信息（采样率/解码器/连接）默认不打，只在出错时留痕
        if (m.msg && (strstr(m.msg, "rror") || strstr(m.msg, "fail") || strstr(m.msg, "Fail")))
          Serial.printf("[audio] %s\n", m.msg);
        break;
      default:
        break;
    }
  };
}

// ---------------------------------------------------------------- 播放
static void uiTick(bool force);   // 定义在后面

// 按键队列（实体定义在下面的 buttons 段，这里先放出来给 warmWait 用）
static volatile uint32_t g_btnPressQ = 0;   // 待处理的"有效按下"次数
static uint32_t takePresses(uint32_t maxN); // 定义在后面

static void pollBootButton(uint32_t now);   // 定义在下面的按键段，warmWait 要用

// 连续"连不上"的台数与"网络疑似断了"的暂停点。playStation 和 loop 都要用，
// 所以提前声明（playStation 里判死台会自动跳下一个，跳太多就歇一会）。
static uint8_t  g_deadTries      = 0;
static uint32_t g_deadPauseUntil = 0;

// 状态行文字覆盖（网络断开这类特殊情况用）。nullptr = 用默认的"连接中 / 播放中"。
// 不加这个的话，断网时屏幕会一直显示"连接中"并停在那一刻 —— 那才是真的像卡死。
static const char* g_statusOv = nullptr;

// 死台记忆：判死过的台在这段时间内直接跳过，不用再花 3 秒去撞连接超时。
// 只记在 RAM（重启即清），而且**会过期** —— 免得一个台只是当时抽风，就永远播不了。
// 死台黑名单时长。以前 5 分钟 —— 但"死"很可能只是 CDN 边缘首连慢（撞满 2.5s
// 超时连败 2 次），5 分钟的代价是用户按过去"一跳跳过 6 个台"。1 分钟足够躲过
// 一次边缘抖动，误判的台 1 分钟后自动恢复候选。
#define DEAD_TTL_MS 60000UL
static uint32_t g_deadUntil[MAX_STATIONS];

// ---------------------------------------------------------------- 主循环看门狗
// 网络异常时 audio 库内部可能长时间阻塞（读 socket 等超时），那种卡死没法从
// 主循环内部恢复 —— 卡住的时候恰恰什么代码都跑不到。这里放一个独立任务盯着：
// 12 秒收不到心跳就重启，让设备自己爬起来，而不是永久死在那里。
//
// ★ 必须 "armed" 之后才生效（loop 跑起来那一刻才武装）：
//   setup 里有合法的长等待 —— netWait() 连 WiFi 最长 15 秒（清配网/换网络时更久），
//   提前生效就是"每 15 秒重启一次"的死循环，设备永远起不来。这个坑踩过一次。
static volatile uint32_t g_loopBeat  = 0;
static volatile bool     g_wdtArmed  = false;

static void loopWatchdogTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!g_wdtArmed) continue;               // setup 阶段不看门
    if (millis() - g_loopBeat > 12000) {
      Serial.println("[wdt] 主循环 12 秒无响应（音频库内部阻塞），强制重启");
      delay(60);
      ESP.restart();
    }
  }
}

// ---------------------------------------------------------------- 网络流畅度
// 右下角"播放中"后面那三个圆点。
// 指标 = 代理层"距上次从上游读到音频数据"的间隔（proxyFeedGapMs）。
// 这是"会不会卡"最直接的信号：CDN 供数一稀疏，解码器存粮就在见底，几百毫秒后
// 就是一次卡顿。用 EMA 平滑掉 CDN 突发式供数带来的毛刺，再分三档。
//   档位 3 流畅（绿）/ 2 一般（黄）/ 1 卡（红）/ 0 未知（不显示：换台中）
static uint8_t  g_netGrade   = 0;
static float    g_netGap     = 0;      // EMA（ms）
static uint32_t g_netValidAt = 0;      // 最近一次拿到有效 gap 的时刻

static void netHealthTick() {
  // 10Hz 采样 + EMA（时间常数 ~0.5 秒）。不能每圈都更新：主循环 2ms 一圈，
  // 那样 EMA 等于没平滑，CDN 突发式供数的毛刺会直接把指示器闪来闪去。
  static uint32_t s_at = 0;
  uint32_t now = millis();
  if (now - s_at < 100) return;
  s_at = now;

  uint16_t gap = proxyFeedGapMs();
  if (gap) {
    g_netValidAt = now;
    g_netGap += 0.2f * ((float)gap - g_netGap);
    // 阈值来自实测：正常播放 gap 在 3~400ms（CDN 按突发发包，几百毫秒的间隔是常态），
    // 断供超过 1 秒解码器存粮才真的见底。所以"流畅"要放宽到 600ms ——
    // 若按直觉设 260ms，正常播放就常亮黄/红，指示器变成狼来了。
    g_netGrade = (g_netGap < 600.0f) ? 3 : (g_netGap < 1300.0f ? 2 : 1);
    return;
  }

  // 拿不到指标（代理在吐存粮/重连，或压根没在转发）。
  // ★ 短暂的一两秒（换台后的吐垫）保持原状就行；但超过 1.5 秒还拿不到，
  //   说明流真的断了 —— 这时必须**显示红点**，不能消失：
  //   屏幕上是"播放中"却没有任何指示，用户只会以为指示器坏了/设备死了。
  //   卡死前最后那一帧恰恰就是这种状态，所以这里必须亮红。
  if (now - g_netValidAt > 1500) {
    g_netGap   = 2000.0f;
    g_netGrade = 1;
  }
}

static bool stationDead(uint16_t i) {
  return i < MAX_STATIONS && g_deadUntil[i] &&
         (int32_t)(millis() - g_deadUntil[i]) < 0;
}

// 从 from 往后走 n 个"没判死"的台（一次跳好几个台时用，别落在死台上）
static uint16_t stepAlive(uint16_t from, uint32_t n) {
  uint32_t cnt = g_list.size();
  if (!cnt) return 0;
  uint16_t base = (uint16_t)(from % cnt);
  uint16_t t    = base;
  for (uint32_t k = 0; k < n; ) {
    t = (uint16_t)((t + 1) % cnt);
    if (!stationDead(t)) ++k;
    if (t == base) break;          // 一圈全是死台，别在这儿死循环
  }
  return t;
}

// 换台前先"预热"目标台：让代理任务（跑在 core 0）把 HTTP 头 + 20KB 音频攒好。
// 这一步是**在旧台还在出声的时候**做的，所以：
//   1. 等待期间不静音 —— 以前 connecttohost 一调进去旧台立刻就没了，然后干等 0.8~2 秒
//   2. 主循环完全自由 —— 界面能动、按键能立刻响应、再按一下就换个目标重来
//   3. 等真的连的时候，代理手里是现成的头和 18KB 数据，audio 那边 130ms 就出声
//
// 返回值：1 = 头已经到手（继续走代理，audio 解析毫秒级）
//         0 = 代理不可用（退回直连，只在 proxyBegin 失败时出现）
//        -1 = 这期间用户又按键了
//        -2 = 这个台连不上（别把 audio 扔进去，直接跳下一个）
//
// 【为什么等的是"响应头"而不是"攒够 18KB"】
// 以前这里等 18432 字节，等于要求"连上就出声"。命中预取时确实爽（130ms），
// 但冷台（开机第一台、一次跳好几个台、预取过期）在 64kbps 下光攒数据就要 2.3 秒，
// 正好烧满预算 → 超时 → 退回直连 → 撞进 audio 库的响应头忙等 → 整机停摆几秒。
// 用户看到的就是"卡死"。
//
// 【为什么必须等"完整的"头，一秒都不能省】
// ESP32-audioI2S 的 parseHttpResponseHeader() 是纯忙等（`while(true)` 里只读不 delay，
// 5 秒超时），而且它**不只**在 connecttohost 里调 —— `Audio::loop()` 的
// HTTP_RESPONSE_HEADER 分支也会调。也就是说：只要 audio 连上来的时候头还没齐，
// 那 1~2 秒的忙等就会落到主循环里，界面彻底不动。所以这段等待必须在这里做，
// 因为这里旧台还在响、按键还能采、秒数还在涨。
//
// 【两个上限，别混用】
//   WARM_BUDGET_MS      —— 软预算，整次换台共用一份；只管"多久还没连上就别等了"。
//   WARM_DEAD_GRACE_MS  —— 连都连不上时再宽限这么久，超过才敢说它是死台。
//   WARM_HEAD_MAX_MS    —— 连上了但头死活不来（服务器抽风）的兜底，别无限等。
// 只用一个上限就会出事：开机第一次连接要同时做 DNS + ARP，1.5 秒超时下会连挂两次，
// 一个好好的台直接被判死跳过（实测第 3 次就通了 —— 现在开机先预热 DNS 避免这个）。
// ★ 这三个上限被"整机自检"压过一轮（tools/stress_test.py，连续换台 10 次）：
//   原来 3000/2000/4500 时，偶发一次换台要 7.8 秒 —— 因为冷路径上是
//   「预热等 3 秒 + 连接等 2.5 秒 + 攒垫 2 秒」三段**串行**，每段都是秒级，叠起来就爆。
//   用户能接受 1~2 秒，不能接受七八秒，所以每段都收紧：
//   预取正常 700~900ms 就绪，1.2 秒还不来基本就是慢台/死台，早连早出声更划算。
#define WARM_BUDGET_MS     1200
#define WARM_DEAD_GRACE_MS 1000
#define WARM_HEAD_MAX_MS   2500

static int warmWait(uint16_t idx, uint32_t budgetMs) {
  if (!proxyReady()) return 0;
  proxySetNext(idx, idx);              // 让代理任务开始暖这个台
  uint32_t t0 = millis();
  uiConnBarReset();
  while (!proxyHasHeader(idx)) {
    audio.loop();                      // ★ 旧台保持解码（已静音），流别断，暖完秒切
    g_loopBeat = millis();             // 预热是正常慢流程，喂心跳别让看门狗误杀
    pollBootButton(millis());          // ★ 这里也要采按键：预热可能几百毫秒
    if (g_btnPressQ) return -1;        // 用户又按了 → 立刻交出控制权，别拦着他

    ProxyWarm w = proxyWarm(idx);
    if (w.fails >= 2) return -2;       // 连续连不上，别再空等

    uint32_t el = millis() - t0;
    // 还没连上：软预算 + 宽限都用完就判死；连上了就再等头（头是必然要来的）
    if (w.state == 0 && el > budgetMs + WARM_DEAD_GRACE_MS) return -2;
    if (el > WARM_HEAD_MAX_MS) return -2;
    uiWaitTick((int)el);               // 秒数在涨、底部条在滑，看着不是死机
    delay(5);
  }
  return 1;
}

// muteFirst：进门前是否先把音量压到 0。
//   换台（有旧台在放）必须 true —— 不然掐流那一下会爆音。
//   开机首播 / 配网成功后开播必须 false —— 那会儿根本没有旧台要静音，
//   而静音之后要等"出声判定"才会淡入；一旦那次判定没触发（连上了但解码器
//   还没吐数据），音量就永远停在 0：现象就是"连上了没声音，换个台才响"。
static void playStation(uint16_t i, bool muteFirst = true) {
  if (g_list.empty()) return;
  uint16_t target = i % g_list.size();

  uint32_t tStart  = millis();
  uint32_t tUi     = 0;      // 按下 → 画面已经换成新台（这才是"按了有没有反应"）
  int      warmRes = 0;

  // ---- ⓪ 按下即静音（用户定的体验规则：不用渐变，新台响起再渐大）----
  // 库内 gain_ramp 会用 ~50ms 把实际增益平滑压到 0，听感就是"咔一下没了"。
  // 从这一刻到 connecttohost 之间预热通常 >150ms，正好把"增益落底 + 残留样本
  // 排空"两件事都在静音里消化掉，掐流时就什么都不响了。
  applyVolume(muteFirst ? 0 : g_vol);
  g_fadeIn = false;
  uint32_t tMute = millis();

  // ---- ① 先把画面切过去：一次整屏重绘 35ms，这是"按下就有反应"的代价 ----
  // round 循环只处理"预热期间用户又改主意"的情况，**总预算共用一份**，
  // 所以无论按多少下，这一跳最多花 WARM_BUDGET_MS。
  for (uint8_t round = 0; round < 3; ++round) {
    g_idx = target;
    // ★ 这里**绝对不能**写 NVS。以前是 prefs.putUShort("idx", g_idx)，位置还在
    //   uiTick(true) 前面，等于"先擦 flash 再刷屏"：NVS 落盘要擦 4KB 扇区，
    //   单次 300~1500ms（写多了更慢）。表现就是"按一下要一两秒屏幕才动"。
    //   现在只打脏标记，等换台停下来 3 秒后由 loop() 慢悠悠写一次。
    g_idxDirty = true;

    g_title[0] = 0;
    g_icyName[0] = 0;
    g_eof = false;
    g_playing = false;
    g_heardSound = false;
    g_tSwitch = millis();
    g_lastBar = 0;

    // ★ 不跳页面：直接在主界面上把台名换成新台、状态改成"连接中"、底条变进度条。
    //   以前这里调 uiMsg() 会整屏换成另一个版式，一进一出两次跳变，看着像在报错。
    uiTick(true);
    if (round == 0) tUi = millis() - tStart;   // 只记第一次：这是"按键→画面变了"的真实延迟

    // ---- ② 预热。旧台保持运行但已静音（按下即静音，见 ⓪），新台响起新声音 ----
    if (proxyCanServe(g_idx)) { warmRes = 1; break; }
    // ★ 预算共用一份，不是每轮各给一次。以前写死 3500 × 最多 3 轮 = 最坏 10.5 秒，
    //   连按几下就能烧出来 —— 那就是"卡死"的数值来源。
    uint32_t used = millis() - tStart;
    warmRes = warmWait(g_idx, used >= WARM_BUDGET_MS ? 0 : (WARM_BUDGET_MS - used));
    if (warmRes != -1) break;

    // 预热期间用户又按了键：把队列取空，重新算目标，接着暖这一轮。
    // 旧台一直在出声，所以"改主意"的代价只是重来一次，不是静音。
    uint32_t q = takePresses(8);
    if (!q) break;
    target = (target + q) % g_list.size();
  }

  uint32_t tWarm = millis() - tStart - tUi;   // 预热花了多久（这段旧台一直在放）
  g_tStopped = millis();

  // ---- ③ 死台：代理明确说连不上，那就别再让 audio 去撞响应头忙等 ----
  // 一个连不上的台走直连的话，parseHttpResponseHeader 会忙等 916~3053ms，
  // 那段时间主循环完全不动 —— 这才是"按一下卡住"的真实手感。
  // 代理连续 2 次 TCP 都连不上（各 1 秒超时）就判死，代价 2.4 秒且全程可响应。
  if (warmRes == -2) {
    g_playing    = false;
    g_tConnected = millis();
    g_lastTry    = g_tConnected;
    g_deadUntil[g_idx < MAX_STATIONS ? g_idx : 0] = millis() + DEAD_TTL_MS;
    // 连续几个台都连不上 = 大概率是网断了，别一层层递归下去（吃栈），就地歇一会
    if (++g_deadTries > 3) {
      g_deadTries      = 0;
      g_deadPauseUntil = millis() + 20000;
      g_statusOv       = "网络断开，稍后重试";
      Serial.println("[play] 连续几个台都连不上，网络大概断了，20 秒后再试");
      // 旧流其实还没掐（connecttohost 没跑），把静音撤掉让它继续响，别白静 20 秒
      applyVolume(g_vol);
      // 把 g_tSwitch 拨到抢跑窗口之外：否则 warming 又成立，uiWaitTick 会不停
      // 往状态行写"连接中 N.Ns"，把"网络断开"这句话盖掉。
      g_tSwitch = millis() - SWITCH_WARM_MS - 1;
      uiTick(true);
      return;
    }
    Serial.printf("[play] %u 号台连不上，跳过（连续第 %u 个）\n",
                  (unsigned)g_idx, (unsigned)g_deadTries);
    playStation(stepAlive(g_idx, 1));
    return;
  }
  g_deadTries = 0;
  g_statusOv  = nullptr;

  // ★ 只要代理在，就永远走代理。
  //   以前是"没暖好就退回直连"，而直连正是唯一会让主循环彻底停摆两秒的路。
  //   现在代理接管的是"已经建好的上游连接 + 已经收到的头"，冷路径也不再是悬崖。
  bool viaProxy = proxyReady();
  String url = viaProxy ? proxyUrlFor(g_idx) : g_list[g_idx].url;

  // ★ 静音已在按下那一刻完成（见 ⓪）。掐流前只需保证"增益已落底 + 解码器销毁
  //   时冲进 DMA 的残留样本（m_validSamples，见库 performAudioTask else 分支）
  //   是静音的"。预热期间这点时间通常早就够了，这里只补足最低 150ms。
  // 只有"真静音过"才需要等增益落底；首播不静音，也就没有这 150ms 的启动延迟
  if (muteFirst) {
    uint32_t sinceMute = millis() - tMute;
    if (sinceMute < 150) delay(150 - sinceMute);
  }

  bool ok = audio.connecttohost(url.c_str());

  // 万一预取在判定之后刚好失效（代理回 503），立刻直连一次兜底
  if (!ok && viaProxy) {
    viaProxy = false;
    ok = audio.connecttohost(g_list[g_idx].url.c_str());
    Serial.println("[play] 代理没兜住，已退回直连");
  }

  g_tConnected = millis();
  g_lastTry = g_tConnected;
  g_playing = ok;
  // ★ 出声基准：以"新连接建立"这一刻为界。之前预热期间旧台还在出样本，
  //   之后的增量才是新流的。见文件顶部 g_sampleBase 的说明。
  g_sampleBase = spectrumSampleCount();
  if (!ok) applyVolume(g_vol);   // 没连上别把用户晾在静音里，音量立即回位

  // 连上了就顺手把"下一个台"交给代理去暖着，等你按键时它已经就绪
  if (ok && proxyReady()) proxySetNext(g_idx, (g_idx + 1) % g_list.size());

  if (!ok) Serial.printf("[play] %u 号台连接失败%s\n", (unsigned)g_idx, viaProxy ? "（代理与直连都试过）" : "");
}

static void nextStation() {
  if (g_list.empty()) return;
  playStation(stepAlive(g_idx, 1));
}

// 这个台不行，跳到下一个。连续跳太多次说明是网络问题，别再疯狂刷台，就地等
static void skipStation() {
  if (g_skipStreak >= SWITCH_MAX_SKIPS) {
    Serial.println("[play] 连跳太多次，大概是网络不通，先停在这个台");
    g_tSwitch = millis();      // 重置计时，9 秒后再试，别原地死循环
    return;
  }
  ++g_skipStreak;
  nextStation();
}

static void applyVolume(int v) {
  if (v < 0) v = 0;
  if (v > VOLUME_STEPS) v = VOLUME_STEPS;
  g_appliedVol = (int8_t)v;
  audio.setVolume((uint8_t)v);
}

static void setVolume(int v) {
  if (v < 0) v = 0;
  if (v > VOLUME_STEPS) v = VOLUME_STEPS;
  if (v == g_vol) return;
  g_vol = (uint8_t)v;
  // 用户在拧音量 = 用户接管，正在进行的淡入立即作废，直接到位
  g_fadeIn = false;
  applyVolume(g_vol);
  g_volDirty = true;      // 松手时才写 NVS，避免按住时狂写 flash
  uiTick(true);
}

// ---------------------------------------------------------------- WiFi + 网页
// 自实现配网 + 常驻网页设置，整套移植自「小米闹钟」项目（原来是 WiFiManager）。
// 为什么换：WiFiManager 的页面是英文的、配网失败只有 "Failed to connect"，
// 而且"改电台表要重启设备"。现在：中文扫网页 + 失败原因逐条提示 +
// 设置页可以在线选台/调音量/改电台表（保存即生效，不重启）。
// 注意 include 位置：wifi_web.h 直接调 playStation/setVolume/buildList/uiTick，
// 必须在这些定义之后。uiTick 有前置声明（147 行），所以放这里没问题。
#include "wifi_web.h"

// WiFi 就绪后的一次性收尾：代理、DNS 预热、NTP、开播。
// visual=true = setup 直连成功路径（推进开机画面）；
// visual=false = 配网成功后从 loop 的 netTask 进入（开机画面早就翻篇了）。
void bootstrapAfterWifi(bool visual) {
  g_wifiOK       = true;
  g_bootstrapped = true;
  Serial.printf("[wifi] %s  RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  if (visual) uiBootStep("正在准备预取", 760);
  if (proxyBegin()) Serial.println("[boot] 预取代理已启动");
  else              Serial.println("[boot] 预取代理未启动，直接连上游");

  // DNS 预热：开机第一次 TCP 连接如果还要同时做 DNS 解析 + ARP，很容易顶掉那条
  // 1.5 秒的连接超时。实测第一台因此连挂两次，被当成"死台"跳过 —— 第 3 次其实就通了。
  // 这里先把蜻蜓FM 的域名解析一次塞进缓存，62 个内置台就都不会再撞这个坑。
  // （自定义台可能在别的域名上，它们首次使用仍要各自解析一次，无法预知。）
  {
    IPAddress ip;
    uint32_t tDns = millis();
    bool okDns = WiFi.hostByName("lhttp.qtfm.cn", ip);
    Serial.printf("[boot] DNS 预热 lhttp.qtfm.cn → %s（%ums）\n",
                  okDns ? ip.toString().c_str() : "失败",
                  (unsigned)(millis() - tDns));
  }

  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.ntsc.ac.cn", "pool.ntp.org");

  if (visual) uiBootStep("正在开始播放", 900);
  if (g_idx >= g_list.size()) g_idx = 0;
  // ★ 首播不静音：没有旧台要盖，静音只会带来"连上了却没声音"的风险
  playStation(g_idx, false);
  uiTick(true);   // visual 路径：沿用原 setup 的收尾节奏；配网路径：从配网页切回主界面
}

// ---------------------------------------------------------------- 按键
// ★ 主按键走**轮询**，不走中断。原因见 buttons.h 顶部：
//   中断那套判据（"必须出现 ≥80ms 稳定高电平" + 中断里读电平判方向）对**串口模拟**
//   按键永远成立（电平是 pinMode(OUTPUT) 硬驱动的，干净单调），所以测试全绿；
//   但对真按键是灾难 —— 松手弹跳刷新高电平起点、中断延迟后读到已恢复的高电平，
//   真实按压被整批当成抖动丢掉。这就是"串口换台丝滑、实体键没法用"。
//
// 记账模型：
//   轮询到"稳定按下"就 g_btnPressQ++，并记下按下的时刻用于测延迟
//   主循环把队列**一次取空**，合并成一次跳转（按 3 下 = 往前跳 3 个台，只连一次）
static uint32_t  g_btnDownAt  = 0;      // 最近一次被采纳的按下（毫秒）
static bool      g_btnHeld    = false;  // 当前是否处于"按住"状态
static BtnPoll   g_btnBoot;
static BtnPoll   g_volUp, g_volDn;

// 长按计时：**只累计真正采到"按下"的那段时间**，每个采样间隔最多算
// BTN_HOLD_SAMPLE_CAP 毫秒。理由见 buttons.h 底部【为什么不给长按用 arm(now) + 墙钟】。
// 一句话：主循环卡住（等响应头、连跳死台）时那段不算数 —— 宁可让人多按一会儿，
// 也绝不能把"卡住的时间"算成"用户按住了"，那会直接删掉 WiFi 密码。
static uint32_t  g_heldMs     = 0;      // 本轮连续按住累计
static uint32_t  g_heldSample = 0;      // 上次采样时刻
static uint8_t   g_holdMark   = 0;      // 已经提示到第几档（0 没提示 / 1 过半 / 2 快到了）

// 主按键自己的消抖窗口（BTN_MAIN_DEBOUNCE_MS，见 config.h）。
// 每轮调一次。稳定 LOW 就算一次按下（按下即触发，不等松手）。
//
static void pollBootButton(uint32_t now) {
  bool down = g_btnBoot.read(now);

  if (down) {
    if (!g_btnHeld) {
      g_btnHeld   = true;
      g_btnDownAt = now;
      g_heldMs     = 0;
      g_heldSample = now;
      g_holdMark   = 0;
      if (g_btnPressQ < BTN_QUEUE_MAX) g_btnPressQ++;
    } else {
      // ★ 只在"这次采样确实看到按下"时累加，间隔上限 BTN_HOLD_SAMPLE_CAP。
      //   主循环被卡 3 秒，这里也只会计 200ms —— 误触的根源就在这里堵死。
      uint32_t d = now - g_heldSample;
      if (d > BTN_HOLD_SAMPLE_CAP) d = BTN_HOLD_SAMPLE_CAP;
      g_heldMs += d;
      g_heldSample = now;
    }
  } else {
    if (g_btnHeld) g_btnHeld = false;
    g_heldMs   = 0;
    g_holdMark = 0;
  }
}

// ---- 换台节奏：平静期立刻跳，连击期合一次跳 ----
// 预取槽只有一个，暖一个台要 700~900ms。按得比这快，每一下都只能冷连接（1~3 秒），
// 撞上死台再加 9 秒 —— "等十几秒"就是这么来的。所以两种节奏区别对待：
//   平静期（距上次换台 > CALM_MS）→ 立刻跳，单按 150ms 出声，一秒都不多等；
//   连击期                      → 等 BURST_MS 把这串收齐，合并成一次跳转，只付一次连接成本。
#define JUMP_CALM_MS   1200   // 距上次换台超过这个时间算"平静"
#define JUMP_BURST_MS   200   // 连击收窗：这么久没有新按键就落地
                              // 200ms 足够吃掉 120ms 节奏的连击（人按不到这么快），
                              // 又不会把"隔半秒按一下"的正常操作也拖慢
static uint32_t g_lastJumpAt   = 0;      // 上次真正换台的时刻
static bool     g_burstPending = false;  // 手上还压着一串连击没落地
static uint32_t g_burstAt      = 0;      // 收窗截止时刻
static uint32_t g_burstCount   = 0;      // 这串攒了几下

// 一次把队列取空。返回这一批攒了几次按下。
// 现在是纯单线程（轮询），不需要临界区了。
static uint32_t takePresses(uint32_t maxN) {
  uint32_t n = g_btnPressQ;
  if (n > maxN) n = maxN;
  g_btnPressQ -= n;
  return n;
}

static void doJump(uint32_t n) {
  if (!n || g_list.empty()) return;
  uint32_t from = g_idx;
  uint32_t to   = stepAlive((uint16_t)from, n);   // 跳过内存里记着的死台
  playStation(to);                                   // 出来时画面已经换好
  g_lastJumpAt   = millis();
  g_burstPending = false;
  g_burstCount   = 0;
}

static void handleButtons() {
  uint32_t now = millis();

  // ★ 先采样按键电平（消抖在这一步完成），再看队列
  pollBootButton(now);

  // ---- BOOT：平静期立刻跳；连击期先收窗，合并成一次跳 ----
  uint32_t q = takePresses(8);
  if (q && !g_list.empty()) {
    // 平静期 = 距上次换台够久，且手上没压着连击 → 立刻响应，零附加延迟
    bool calm = !g_burstPending && (uint32_t)(now - g_lastJumpAt) >= JUMP_CALM_MS;
    if (calm) {
      doJump(q);
    } else {
      // 连击期：把这一串攒起来，350ms 内没有新按键就一次性落地
      g_burstCount += q;
      g_burstPending = true;
      g_burstAt = now + JUMP_BURST_MS;
    }
    now = millis();
  }
  // 收窗到点 → 落地
  if (g_burstPending && (int32_t)(now - g_burstAt) >= 0) {
    doJump(g_burstCount);
    now = millis();
  }
  // ---- BOOT 长按：连续按住 8 秒 → 清配网 ----
  // ★ 判据是"g_heldMs（实际采到的连续按住时长）"，不是"The 墙钟减去按下时刻"。
  //   老实现用后者，主循环一卡（等响应头 2~5 秒、连跳死台十几秒）就够 5 秒，
  //   一次普通按压直接把 WiFi 密码清了 —— 用户遇到的就是这个。
  //   再加一道保险：真到点这一刻，现读一次电平，必须是低。
  if (g_btnHeld && g_heldMs > 0) {
    if (g_heldMs >= BTN_LONGPRESS_MS && btnLevel(PIN_BTN_BOOT)) {
      Serial.println("[btn] 长按 8 秒 → 清除配网");
      resetWiFiAndReboot();
    }
  }

  // 音量：按下立刻加一格，按住 400ms 后每 150ms 继续加（消抖后判定，防抖成连发）
  bool up = g_volUp.read(now);
  bool dn = g_volDn.read(now);
  int8_t dir = up ? 1 : (dn ? -1 : 0);
  if (dir == 0) {
    if (g_volDir != 0 && g_volDirty) {
      prefs.putUChar("vol", g_vol);
      g_volDirty = false;
    }
    g_volDir = 0;
  } else if (dir != g_volDir) {
    g_volDir = dir;
    setVolume(g_vol + dir);
    g_volRepeatAt = now + 400;
  } else if ((int32_t)(now - g_volRepeatAt) > 0) {
    setVolume(g_vol + dir);
    g_volRepeatAt = now + 150;
  }
}

// ---------------------------------------------------------------- 界面刷新
static void uiTick(bool force) {
  // ★ 配网模式：屏幕必须停在配网画面（热点名 + 192.168.4.1）。
  //   以前这里照常画主界面，把 startPortal() 刚画好的配网页整屏盖掉 ——
  //   用户清配网后看到的是"播放中"却没声音，还以为只有一半功能坏了。
  //   每 1.5 秒重画一次，防止被局部绘制残留干扰。
  if (apActive && !g_bootstrapped) {
    static uint32_t s_portalAt = 0;
    if (force || millis() - s_portalAt > 5000) {   // 5 秒一次：整屏 push 要 30ms，别去抢 HTTP 的带宽/CPU
      s_portalAt = millis();
      uiPortal(AP_NAME);
    }
    return;
  }

  uint32_t now = millis();
  if (!force && now - g_lastUi < 400) return;
  g_lastUi = now;

  char clk[8] = "-:--";
  struct tm t;
  if (getLocalTime(&t, 0)) strftime(clk, sizeof(clk), "%H:%M", &t);

  // 注意：不能用 g_playing 判断"在放"—— connecttohost() 一 TCP 连上就把它置 true 了，
  // 但那时候服务器还没发一个字节。真正出声看 g_heardSound（已解出码率）。
  bool conn = !g_heardSound;
  const char* status;
  if (conn)               status = g_statusOv ? g_statusOv : "连接中";
  else                    status = "播放中";

  UiState s;
  s.station = g_list.empty() ? "--" : g_list[g_idx].name.c_str();
  s.title   = g_title;
  s.status  = status;
  s.idx     = g_idx + 1;
  s.total   = (uint16_t)g_list.size();
  s.vol     = g_vol;
  s.volMax  = VOLUME_STEPS;
  s.rssi    = g_wifiOK ? WiFi.RSSI() : 0;
  s.wifiOk  = g_wifiOK;
  s.clock   = clk;
  s.connecting = conn;
  s.battPct    = (int8_t)batteryPercent();
  s.charging   = batteryCharging();
  s.netGrade   = g_netGrade;
  uint32_t el = g_tSwitch ? (now - g_tSwitch) : 0;
  s.progress = (uint16_t)((el * 1000 / 1200 > 1000) ? 1000 : (el * 1000 / 1200));
  uiRender(s);
}

// 复位原因：判断"卡死"到底是崩溃、看门狗、还是掉电的最直接证据。
// 别小看这一行 —— 用户说"卡死"时，光看现象分不清是程序挂住还是它自己重启了。
static const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "上电";
    case ESP_RST_EXT:       return "外部复位脚";
    case ESP_RST_SW:        return "软件重启";
    case ESP_RST_PANIC:     return "★程序崩溃(panic)";
    case ESP_RST_INT_WDT:   return "★中断看门狗";
    case ESP_RST_TASK_WDT:  return "★任务看门狗";
    case ESP_RST_WDT:       return "★其它看门狗";
    case ESP_RST_DEEPSLEEP: return "深睡唤醒";
    case ESP_RST_BROWNOUT:  return "★掉电/供电不足(brownout)";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB 复位（烧录/串口）";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "?";
  }
}

// ---------------------------------------------------------------- setup
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Dayu-radio (源码版) ===");
  Serial.printf("[boot] 复位原因: %d (%s)\n",
                (int)esp_reset_reason(), resetReasonStr(esp_reset_reason()));
  Serial.printf("[boot] chip=%s rev=%d cores=%d flash=%uMB psram=%u heap=%u\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());

  prefs.begin("radio", false);
  g_vol = prefs.getUChar("vol", VOLUME_DEFAULT);
  g_idx = prefs.getUShort("idx", 0);

  uiBegin();                       // 里面会先直写开机画面，再决定用不用 sprite

  if (!psramFound()) {
    uiMsg("未检测到 PSRAM", "编译选项要设 PSRAM=opi");
    Serial.println("[boot] 未检测到 PSRAM，音频库无法工作");
    delay(4000);
  }

  uiBootStep("正在读取电台表", 220);
  buildList(prefs.getString("custom", ""));
  Serial.printf("[boot] 电台表 %u 个台\n", (unsigned)g_list.size());

  // 音频初始化（I2S 引脚固定）
  uiBootStep("正在初始化音频", 400);
  audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  audio.setVolumeSteps(VOLUME_STEPS);
  audio.setVolume(g_vol);
  g_appliedVol = (int8_t)g_vol;   // 坡道起点与实际音量对齐（NVS 里存的值可能 ≠ 默认值）
  // ★ 固定输出采样率 48kHz：44.1k 的源由库内重采样器消化。不固定的话，在
  //   44.1k/48k 两种台之间切换会触发 I2S 时钟重配（disable→enable），外部功放
  //   休眠再唤醒 = 换台时一声模拟域爆音，软件静音盖不住。配合 Audio.cpp 里
  //   setSampleRate() 的两处补丁（同率不动时钟 / 固定输出率只更新重采样器），
  //   I2S 时钟从此永不断流。
  audio.setOutputSampleRate(Audio::SR_48000);
  // ★ 连接/读超时不能给长：网络差时 audio.loop()/connecttohost() 内部读 socket
  //   会一直等到超时才返回，那段时间主循环是死的 —— 屏幕不动、按键全不响应
  //   （用户报的"网络不好就卡死"就是这个）。原来 4000/8000，一次拥塞能冻 8~12 秒；
  //   现在 2500/1500：64kbps 流每 100~200ms 就来一个包，1.5 秒收不到数据基本就是
  //   断了，交给重连反而更快。卡死上限从 12 秒压到 4 秒。
  audio.setConnectionTimeout(2500, 1500);
  setupAudioCallback();
  spectrumBegin();   // 频谱：窗函数 + 色带 + 局部 sprite（PCM 从库的 audio_process_i2s 钩子来）
  // 恢复网页里选的风格。repaint=false：这时屏幕还是开机画面，不能往上推频谱块
  spectrumSetStyle(prefs.getUChar("spec", SPEC_HEAT), false);
  batteryBegin();    // 电池：GPIO17 分压采样 + GPIO38 充电检测

  // 按键
  g_volUp.begin(PIN_BTN_VOLUP);
  g_volDn.begin(PIN_BTN_VOLDN);
  // BOOT 走轮询消抖（不挂中断）。理由见 buttons.h 顶部 ——
  // 中断那套判据对真按键的弹跳不成立，会把真实按压整批当抖动吞掉。
  g_btnBoot.begin(PIN_BTN_BOOT, BTN_MAIN_DEBOUNCE_MS);

  // 主循环看门狗：网络异常时音频库内部阻塞会让主循环彻底停摆，
  // 那种情况下只有外部任务能救场（12 秒没心跳就重启）
  g_loopBeat = millis();
  xTaskCreatePinnedToCore(loopWatchdogTask, "loopWdt", 3072, nullptr, 1, nullptr, 0);

  // 配网（自实现，见 wifi_web.h）。三种情况：
  //   ① 有自己存的凭据 → 直接连，15 秒内连上就走开机画面继续；
  //   ② 没有凭据 → 先试 WiFi.begin() 无参（吃老版 WiFiManager 存在系统 NVS
  //      里的凭据），连上就把凭据迁到自己的 NVS —— 老用户升级无感；
  //   ③ 都不行 → 起热点配网，**不重启**：屏幕显示配网信息，netTask 在
  //      loop 里等用户配好，连上后自动继续（bootstrapAfterWifi）。
  //      原来这里失败就 ESP.restart()，等于"连不上就永远重启"。
  uiBootStep("正在连接 WiFi", 560);
  WiFi.persistent(false);          // 凭据我们自己存 NVS（迁移读取不受影响）
  WiFi.setAutoReconnect(false);    // 重连由 netTask 统一管理，别让驱动自己抢
  loadWifiCreds();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);            // 关省电：音频流的延迟优先

  bool wifiConnected = false;
  if (staSSID[0]) {
    WiFi.begin(staSSID, staPass);
    wifiConnected = netWait(WIFI_CONNECT_TIMEOUT_MS);
    if (!wifiConnected) {
      lastFailCode = (int)WiFi.status();
      WiFi.disconnect();           // 断开即可：别关 wifi，马上要开热点
    }
  } else if (prefs.getBool("wmigrated", false) == false) {
    WiFi.begin();                  // 无参 = 用系统 NVS 里已有的凭据（若有）
    if (netWait(8000)) {
      saveWifiCreds(WiFi.SSID().c_str(), WiFi.psk().c_str());
      prefs.putBool("wmigrated", true);   // 只迁一次，清配网后别再从系统区迁回来
      Serial.printf("[boot] 已迁移旧配网凭据：%s\n", staSSID);
      wifiConnected = true;
    } else {
      WiFi.disconnect();
    }
  }

  // Web 服务必须在 WiFi 初始化之后（WebServer 底下是 esp_http_server，依赖 lwIP）
  webSetup();

  if (wifiConnected) {
    bootstrapAfterWifi(true);
    uiTick(true);                  // 联网成功才画主界面
  } else {
    if (lastFailCode) Serial.printf("[boot] WiFi 连接失败 code=%d，转热点配网\n", lastFailCode);
    // startPortal() 内部已经画了配网画面 —— 这里绝不能再 uiTick(true)，
    // 那会把配网页整屏盖成"播放中"。
    startPortal();
  }
}

// ---------------------------------------------------------------- loop
void loop() {
  // 第一圈 loop 才武装看门狗：setup 里的连接等待是合法的，不能算卡死
  if (!g_wdtArmed) { g_wdtArmed = true; }
  g_loopBeat = millis();          // 喂主循环看门狗（armed 后 12 秒没跳动就重启）

  // ★ 按键必须排在最前面。audio.loop() 要解包、要往 I2S 搬数据，抢跑窗口里
  //   还要连喂 10 次 —— 排在它后面就等于"按一下要等数据喂完才认"。
  //   实测把 10 次 pump 排在按键前面，能拖出 1~2 秒的"按了没反应"。
  handleButtons();

  // 网页服务 + 配网状态机。空闲时只有几微秒；有请求时处理几~几十毫秒，
  // 由 lwIP 的接收缓冲兜着，换台抢跑窗口少转两圈也不会丢请求。
  // DNS 捕获门户必须每圈喂，否则手机连热点后弹不出配置页。
  webLoop();

  // ★ 配网模式：主循环专心喂 HTTP —— 手机加载配网页的速度直接取决于它。
  //   ★ 但只在"还没配好"时让路：一旦配网成功（g_bootstrapped）设备已经开始放歌，
  //     这时候再走短路就等于不喂 audio.loop()，声音出不来、界面也停在配网页，
  //     用户看到的就是"连上了却卡住"（真实反馈）。热点的关闭交给 netTask 计时。
  if (apActive && !g_bootstrapped) {
    uiTick(false);      // 内部按 5 秒节流维护配网画面
    delay(1);
    return;
  }

  // 频谱律动（内部 80ms 节流；流断/静音时自动画空，开销 ~1ms/帧）
  spectrumTick();

  // 电池/充电（内部 500ms/15s 节流；状态变化才刷屏）
  if (batteryTick()) uiTick(true);

  // 网络流畅度（三格信号条）：每圈几微秒，内部做过 EMA
  netHealthTick();

  // 切台淡入：新台出声后每 55ms 往上爬 1 格，直到回到用户音量（21 格 ≈ 1.1 秒，
  // 用户实测 0.6 秒仍有残留劣化感，要求过渡 1~1.5 秒）。
  // 放在按键前面：它只花几微秒，但保证坡道不吃任何卡顿的影响。
  if (g_fadeIn) {
    if (g_appliedVol >= g_vol) {
      g_fadeIn = false;
    } else {
      uint32_t nowF = millis();
      if (nowF - g_fadeAt >= 55) {
        g_fadeAt = nowF;
        applyVolume(g_appliedVol + 1);
      }
    }
  } else if (g_appliedVol == 0 && !g_heardSound && g_tSwitch && millis() - g_tSwitch > 3000) {
    // ★ 静音兜底：换台把音量压到 0 之后，淡入只认"出声判定"那一条路。
    //   判据用 !g_heardSound（没听到声）而不是 !g_playing（没连上）：
    //   真正会卡静音的场景恰恰是"TCP 连上了、decoder 却一直不出数据"——
    //   那时 g_playing 早就是 true，用 !g_playing 根本兜不住。
    //   宁可少一次渐入，也不能让用户对着一个静音的收音机。
    applyVolume(g_vol);
  }

  // 换台后的"抢跑窗口"：这段时间里数据还没到齐，主循环唯一有价值的事就是
  // 多喂几次 audio.loop()（每次都读一次 socket），并把界面重绘让位给它。
  bool warming = !g_heardSound && (millis() - g_tSwitch < SWITCH_WARM_MS);

  if (warming) {
    uint32_t tSw = g_tSwitch;
    for (uint8_t i = 0; i < SWITCH_PUMP_LOOPS; ++i) {
      audio.loop();
      // 每喂一次就回头看一眼按键。这一串 pump 加起来可能几十毫秒，
      // 攒到最后再处理，就是"连按时后面几下没反应"的来源。
      if (g_btnPressQ) {
        handleButtons();
        if (g_tSwitch != tSw) break;   // 已经又换了一个台，剩下的 pump 没意义
      }
    }
    // 等待期间的动效：秒数在涨 + 底部条来回滑。
    // 以前这里只把进度条填满就停住，看着和"卡死"没区别。
    uint32_t el = millis() - g_tSwitch;
    if (el - g_lastBar >= 50) {
      g_lastBar = el;
      uiWaitTick((int)el);
    }
  } else {
    audio.loop();
  }

  // 串口命令（只保留运维/产测会用到的四条）
  //   n = 下一个台（和按 BOOT 同一条路径）
  //   d = 把当前画面按 RGB565 原始帧吐到串口（核版式、截图用）
  //   w = 清除配网并重启
  //   e = 电池/充电诊断（电压、引脚电压、充电脚电平）
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'n' || c == 'N') {
      nextStation();
    } else if (c == 'd' || c == 'D') {
      uiDump(Serial);
    } else if (c == 'w' || c == 'W') {
      Serial.println("[cmd] 清除配网并重启");
      resetWiFiAndReboot();
    } else if (c == 'e' || c == 'E') {
      Serial.printf("[batt] 充电脚%d=%d（%s）  电池 %umV  引脚 %umV  %d%%\n",
                    PIN_CHG_DET, digitalRead(PIN_CHG_DET),
                    batteryCharging() ? "充电中" : "用电池",
                    batteryMilliVolts(), batteryPinMilliVolts(), batteryPercent());
    }
  }

  if (audio.isRunning()) g_playing = true;

  // 第一次真的把样本送进 I2S，才算"活儿干完了、已经在响"。
  // ★ 这里以前用 audio.getBitRate() > 0 —— 但音频库在内缓冲只有 128 字节时就
  //   会建解码器、解析出 MP3 帧头，码率立刻 > 0，而真正输出还要再攒够起播门槛
  //   （18KB 左右）。也就是说码率 > 0 只代表"摸到了帧头"。
  //   一旦流僵在"建了解码器、但样本一个都没出来"的地方（CDN 半连接、代理刚吐完
  //   头部就断供……），界面会显示"播放中"、淡入也做了，可喇叭是全静音；更糟的是
  //   判成"已出声"之后，9 秒放弃和自动跳台这两条自动恢复路径全被跳过 ——
  //   只能人工按一下下一个台。这就是"有时开机没声音，按一下换台才有"。
  //   现在只看真实样本（spectrumSampleCount），骗不了人。
  if (!g_heardSound && g_playing && spectrumSampleCount() != g_sampleBase) {
    g_heardSound = true;
    g_failTries = 0;
    g_skipStreak = 0;
    g_deadTries  = 0;
    g_fadeIn     = true;           // 出声了：从 0 开始往用户音量爬（loop 里每 55ms 一格）
    g_fadeAt     = 0;
    Serial.printf("[play] %u/%u %s  出声 %ums\n",
                  (unsigned)(g_idx + 1), (unsigned)g_list.size(),
                  g_list.empty() ? "--" : g_list[g_idx].name.c_str(),
                  (unsigned)(millis() - g_tSwitch));
    uiTick(true);          // 终于能显示了，立刻把"播放中"刷出来
  }

  // ---- 连接状态看护：连不上或一直没声音，别让机器干等着 ----
  if (g_eof) {
    // 掉线重连（不跳页：界面原地退回"连接中 + 进度条"）
    g_eof = false;
    g_playing = false;
    g_heardSound = false;
    g_tSwitch = millis();
    uiTick(true);
    delay(600);
    playStation(g_idx);
  } else if (!g_heardSound) {
    uint32_t since = millis() - g_tSwitch;
    // 网络疑似断了：这段时间谁也别去碰 audio（碰了就是几秒的忙等）
    bool paused = (int32_t)(millis() - g_deadPauseUntil) < 0;
    if (!g_playing) {
      // TCP 根本没连上：SWITCH_RETRY_MS 后重试，同一个台试 SWITCH_MAX_TRIES 次
      if (since > SWITCH_RETRY_MS && !paused) {
        if (++g_failTries >= SWITCH_MAX_TRIES) { g_failTries = 0; skipStation(); }
        else                                    { playStation(g_idx); }
      }
    } else if (since > SWITCH_GIVEUP_MS) {
      // TCP 连上了但一个字节音频都没有：这个台大概停播了，自动跳下一个
      Serial.printf("[play] %u 号台 %us 没声音，跳过\n", (unsigned)g_idx, (unsigned)(since / 1000));
      skipStation();
    }
  }

  // ---- 空播看门狗：以为在播，其实样本停了 ----
  // 这是"开机没声音、按一下下一个台才响"的自动兜底：不管僵在哪一层（解码器卡在
  // 半帧、代理吐完头就断供、I2S 侧停摆），只要样本计数不再增长，就跟用户按一下
  // 换台完全等价地重来一次：stopSong() 把库里那套状态机整个清掉，再重新连。
  // 判据用样本计数而不是音量/码率 —— 音量 0 时送的是全零样本，计数照样在涨，
  // 所以静音坡道不会误触发；反过来"有码率没样本"正是要抓的病。
  // 重连两次仍无数据 = 这个台的流本身是死的，换下一个（不再无限重连）。
  {
    static uint32_t s_lastCnt   = 0;
    static uint32_t s_lastCntAt = 0;
    static uint8_t  s_airTries  = 0;

    uint32_t cnt = spectrumSampleCount();
    if (cnt != s_lastCnt) {          // 有样本在出 = 正常，一切归零
      s_lastCnt   = cnt;
      s_lastCntAt = millis();
      s_airTries  = 0;
    }

    bool paused = (int32_t)(millis() - g_deadPauseUntil) < 0;
    // 基准取"最近一次有样本"和"本次换台起点"里较晚的那个：换台后一直没样本时
    // 从换台那一刻起算，不会拿上一个台的安静时间冤枉新台。
    uint32_t refAt = ((int32_t)(s_lastCntAt - g_tSwitch) > 0) ? s_lastCntAt : g_tSwitch;
    // 每失败一次就把等待时间翻倍（4s → 8s → 放弃）：慢网络下"首包要 5 秒"的台
    // 不该被 4 秒的急性子判死，但也不能无限等 —— 第二次就给它 8 秒。
    uint32_t waitMs = DEAD_AIR_MS << (s_airTries > 2 ? 2 : s_airTries);
    if (g_bootstrapped && g_playing && g_tSwitch && !paused &&
        (uint32_t)(millis() - refAt) > waitMs) {
      if (s_airTries >= 2) {
        Serial.printf("[play] %u 号台重连后仍没有音频数据，跳过\n", (unsigned)g_idx);
        s_airTries  = 0;
        s_lastCntAt = millis();
        skipStation();
      } else {
        ++s_airTries;
        Serial.printf("[play] %u 号台 %ums 没有音频数据（音量 %d）→ 自动重连（第 %u 次）\n",
                      (unsigned)g_idx, (unsigned)(millis() - refAt),
                      (int)g_appliedVol, (unsigned)s_airTries);
        audio.stopSong();              // 清掉库里的解码器/连接状态
        g_heardSound = false;
        g_playing    = false;
        g_eof        = false;
        s_lastCntAt  = millis();
        // 已经没有声音在放了，不需要静音坡道（muteFirst=false），直接重连
        playStation(g_idx, false);
      }
    }
  }

  if (g_titleDirty) {
    g_titleDirty = false;
    uiTick(true);
  }

  // 换台停下来 3 秒后再把台号落盘。NVS 写是这条链上唯一会阻塞几百毫秒的操作，
  // 绝对不能放在按键→刷屏的路径里（见 playStation 里的说明）。
  if (g_idxDirty && (uint32_t)(millis() - g_lastJumpAt) >= 3000) {
    g_idxDirty = false;
    prefs.putUShort("idx", g_idx);
  }

  // 抢跑期间不做整屏重绘：一次 240x240 全屏刷要 30ms 左右，正好卡在等数据的关键路径上
  if (!warming) {
    uiTick(false);
  }

  delay(warming ? SWITCH_SPIN_DELAY : 2);
}
