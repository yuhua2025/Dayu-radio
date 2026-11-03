// ============================================================================
//  ui.cpp —— 界面绘制（Dayu-radio）
//
//  历史坑（务必别踩回去）：
//    u8g2 的 wqy*_t_gb2312 字体是 GB2312 编码，LovyanGFX 的 U8g2font 只支持
//    Unicode 编码，拿它查汉字会在字库里无界扫描导致死循环 —— 现象是
//    "背光亮、屏幕全黑、整机不启动"。现在统一用 cnfont.cpp 的自绘点阵字。
//
//  另外 lcd.init() 之后立刻直写一屏启动画面，不依赖 sprite：
//  哪怕后面 PSRAM / 音频 / WiFi 哪一步挂了，屏幕上一定有东西。
//
//  配色：竖向渐变（上深靛蓝 C_BG_TOP → 下夜蓝 C_BG_BOT）。
//  不要再退回 fillScreen(纯黑)，在 IPS 上那就是一块死黑，看着像没点亮。
// ============================================================================
#include "ui.h"
#include "cnfont.h"
#include "spectrum.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

LGFX lcd;
static lgfx::LGFX_Sprite spr(&lcd);

static lgfx::LovyanGFX* tgt       = nullptr;  // 当前绘制目标（sprite 或屏幕）
static bool             useSprite = false;

static void present();   // 前向声明（uiBootStep 要用）

// ---------------------------------------------------------------- 颜色工具
// 565 通道线性插值，t = 0..256
static inline uint16_t lerp565(uint16_t a, uint16_t b, int t) {
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + (((br - ar) * t) >> 8);
  int g = ag + (((bg - ag) * t) >> 8);
  int l = ab + (((bb - ab) * t) >> 8);
  if (r < 0) r = 0; if (r > 31) r = 31;
  if (g < 0) g = 0; if (g > 63) g = 63;
  if (l < 0) l = 0; if (l > 31) l = 31;
  return (uint16_t)((r << 11) | (g << 5) | l);
}

// 背景在 y 处的颜色 —— 小块擦除（比如进度条那一行）必须用它，否则会留一块补丁
static uint16_t bgAt(int y) {
  if (y < 0) y = 0;
  if (y >= SCR_H) y = SCR_H - 1;
  return lerp565(C_BG_TOP, C_BG_BOT, y * 256 / (SCR_H - 1));
}

uint16_t uiBgAt(int y) { return bgAt(y); }   // 导出给 spectrum.cpp 等直写模块用

lgfx::LGFX_Sprite& uiSpriteGet() { return spr; }

// 竖向渐变：每 4px 一条。相邻两条色差 0.3 级，肉眼完全看不出分带，
// 但 fillRect 调用次数从 240 降到 60，省下来的正是换台关键路径上的时间。
static void fillVGrad(lgfx::LovyanGFX* g) {
  const int band = 4;
  for (int y = 0; y < SCR_H; y += band) {
    int h = (y + band > SCR_H) ? (SCR_H - y) : band;
    g->fillRect(0, y, SCR_W, h, lerp565(C_BG_TOP, C_BG_BOT,
                                        (y + h / 2) * 256 / (SCR_H - 1)));
  }
}

// 横向渐变（只用在 4px 高的品牌细线上）：每 8px 一段
static void fillHGrad(lgfx::LovyanGFX* g, int x, int y, int w, int h,
                      uint16_t c0, uint16_t c1) {
  const int step = 8;
  for (int i = 0; i < w; i += step) {
    int ww = (i + step > w) ? (w - i) : step;
    g->fillRect(x + i, y, ww, h, lerp565(c0, c1, i * 256 / (w - 1)));
  }
}

// ---------------------------------------------------------------- 开机画面
// 版式（240x240）：
//   y=0     顶部品牌细线（青→蓝横向渐变）
//   y=62    Dayu-radio        FreeSansBold18pt
//   y=100   64px 品牌短线
//   y=116   网络收音机         20px
//   y=160   调谐刻度（25 根，长的深、短的浅）—— 一眼看出这是台收音机
//   y=186   正在启动…          16px
//   y=210   进度条
//   y=236   底部品牌细线
static const int PROG_X = 40, PROG_Y = 210, PROG_W = 160, PROG_H = 5;

static void drawDial(lgfx::LovyanGFX* g, int cx, int yBottom) {
  const int n = 25, gap = 8;
  int x0 = cx - (n - 1) * gap / 2;
  for (int i = 0; i < n; ++i) {
    bool tall = (i % 6 == 0);
    int h = tall ? 10 : 4;
    g->fillRect(x0 + i * gap - 1, yBottom - h, 2, h, tall ? C_DIM : C_LINE);
  }
}

static void renderBoot(lgfx::LovyanGFX* g, const char* status, int permille) {
  fillVGrad(g);

  // 顶部 / 底部品牌细线
  fillHGrad(g, 0, 0, SCR_W, 4, C_ACCENT, C_ACCENT2);
  fillHGrad(g, 0, SCR_H - 4, SCR_W, 4, C_ACCENT, C_ACCENT2);

  // 品牌名（GFXfont，纯 ASCII，字重够）
  g->setFont(&lgfx::fonts::FreeSansBold18pt7b);
  g->setTextColor(C_TEXT);
  const char* brand = BRAND_NAME;
  g->drawString(brand, (SCR_W - g->textWidth(brand)) / 2, 60);

  // 品牌短线：左段青、右段蓝（跟大字留出呼吸，不然像下划线）
  int rw = 64, rx = (SCR_W - rw) / 2;
  g->fillRect(rx, 104, rw / 2, 3, C_ACCENT);
  g->fillRect(rx + rw / 2, 104, rw - rw / 2, 3, C_ACCENT2);

  // 中文副标题
  const char* sub = BRAND_NAME_CN;
  int sw = cnTextWidth(sub, CN_SIZE_BIG);
  cnDraw(g, sub, (SCR_W - sw) / 2, 120, C_DIM, CN_SIZE_BIG);

  drawDial(g, SCR_W / 2, 170);

  // 状态行
  if (status && *status) {
    int tw = cnTextWidth(status, CN_SIZE_SMALL);
    cnDraw(g, status, (SCR_W - tw) / 2, 186, C_DIM, CN_SIZE_SMALL);
  }

  // 进度条
  g->fillRoundRect(PROG_X, PROG_Y, PROG_W, PROG_H, PROG_H / 2, C_PANEL_HI);
  if (permille < 0)    permille = 0;
  if (permille > 1000) permille = 1000;
  int fw = PROG_W * permille / 1000;
  if (fw > 2) g->fillRoundRect(PROG_X, PROG_Y, fw, PROG_H, PROG_H / 2, C_ACCENT);
}

// ---------------------------------------------------------------- 初始化
void uiBegin() {
  Serial.printf("[ui] 屏幕 SCL=%d MOSI=%d DC=%d CS=%d RST=%d BL=%d invert=%d rot=%d\n",
                PIN_LCD_SCL, PIN_LCD_SDA, PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_RST, PIN_LCD_BL,
                (int)LCD_INVERT, LCD_ROTATION);

  lcd.init();
  lcd.setRotation(LCD_ROTATION);
  lcd.setBrightness(210);

  // ★ 直写一屏开机画面：证明"面板 + SPI 引脚 + 驱动 IC + 字库"全通
  renderBoot(&lcd, "正在启动", 60);

  // 再造 sprite（有就用它，没有就继续直写）
  spr.setColorDepth(16);
  if (psramFound()) {
    spr.setPsram(true);
    useSprite = spr.createSprite(SCR_W, SCR_H);
  }
  if (!useSprite) {
    spr.deleteSprite();
    spr.setPsram(false);
    useSprite = spr.createSprite(SCR_W, SCR_H);
  }
  tgt = useSprite ? (lgfx::LovyanGFX*)&spr : (lgfx::LovyanGFX*)&lcd;

  if (!useSprite) Serial.println("[ui] sprite 分配失败，改用直写");
}

void uiBootStep(const char* status, int permille) {
  if (!tgt) return;
  renderBoot(tgt, status, permille);
  present();
}

static void present() {
  if (useSprite) spr.pushSprite(0, 0);
}

// ---------------------------------------------------------------- 换台进度条
// 换台那 0.5~1.0 秒里，真正在等的是电台服务器把第一段音频攒够，改代码压不下去。
// 这里**不切页面**，就在主界面原地把音量条那一小块变成进度条。
// 只直写 212x4，约 1ms，不占关键路径。
static int s_barLast   = -1;
static int s_waitTenth = -1;   // 上次画的"连接中 N.Ns"，避免每轮都重画同一串
static int s_slideX    = -1;   // 滑动条上一帧的 x

void uiConnBarReset() { s_barLast = -1; s_waitTenth = -1; s_slideX = -1; }

void uiDump(Stream& out) {
  if (!useSprite) { out.println("NOSPRITE"); return; }
  static uint16_t* buf = nullptr;
  const size_t bytes = (size_t)SCR_W * SCR_H * 2;
  if (!buf) buf = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!buf) { out.println("NOMEM"); return; }
  // 先把整帧拷出来，避免往串口吐的过程中界面被重画，导致图像撕裂
  spr.readRect(0, 0, SCR_W, SCR_H, buf);
  // ★ 头部用显式 \r\n，且**结尾不带换行**：紧跟其后的就是一个字节不差的 RGB565。
  //   以前写的是 "\nDUMP ...\n"，USB-CDC 把 \n 扩成 \r\n，抓包端只剥一个字节，
  //   整帧就错位 1 字节 —— 表现为颜色全乱、底部一条花屏。别改回去。
  out.printf("\r\nDUMP %d %d ", SCR_W, SCR_H);
  out.write((const uint8_t*)buf, bytes);
  out.print("\r\nENDDUMP\r\n");
}

// 主界面底部那根条：外框 (12,206,216,8)，内框 (14,208,212,4)
static const int BAR_X = 12, BAR_Y = 206, BAR_W = SCR_W - 24, BAR_H = 8;

void uiConnBar(int permille) {
  if (!tgt) return;
  if (permille < 0)    permille = 0;
  if (permille > 1000) permille = 1000;
  if (permille == s_barLast) return;
  s_barLast = permille;

  const int x = BAR_X + 2, y = BAR_Y + 2, w = BAR_W - 4, h = BAR_H - 4;
  lcd.fillRect(x, y, w, h, bgAt(y));          // 用底色擦，不能写死 C_BG
  int fw = (w * permille) / 1000;
  if (fw > 0) lcd.fillRect(x, y, fw, h, C_ACCENT);
}

// ---------------------------------------------------------------- 连台等待动效
// 服务器什么时候吐第一个字节完全不受控（150~1650ms，死台还会更久）。
// 界面要是这期间一动不动，用户就只会得出一个结论："卡死了"。
// 所以这里做两件事，都只重画很小的两块，约 2ms，不拖慢连接：
//   1. 状态文字变成"连接中 1.4s"（秒数在涨 = 机器还活着）
//   2. 底部那根条 1.2 秒填满后改成来回滑动的指示条（没有终点的等待不要假装有终点）
void uiWaitTick(int elapsedMs) {
  if (!tgt) return;
  if (elapsedMs < 0) elapsedMs = 0;

  // ---- 状态行：固定盒子擦掉再右对齐重画，免得位数变化留下残影 ----
  // 位置在音量条下方（218 起），别碰频谱区（172~204）
  int tenth = elapsedMs / 100;
  if (tenth != s_waitTenth) {
    s_waitTenth = tenth;
    char buf[24];
    snprintf(buf, sizeof(buf), "连接中 %d.%ds", tenth / 10, tenth % 10);
    const int y = 218, boxW = 120, boxH = 17;
    const int bx = SCR_W - 12 - boxW;
    for (int yy = y; yy < y + boxH; ++yy) lcd.drawFastHLine(bx, yy, boxW, bgAt(yy));
    int w = cnTextWidth(buf, CN_SIZE_SMALL);
    cnDraw(&lcd, buf, SCR_W - 12 - w, y, C_WARN, CN_SIZE_SMALL);
  }

  // ---- 底部条：前 1.2s 老老实实填进度，之后换成来回滑动 ----
  const int x = BAR_X + 2, y = BAR_Y + 2, w = BAR_W - 4, h = BAR_H - 4;
  if (elapsedMs < 1200) {
    uiConnBar(elapsedMs * 1000 / 1200);
    return;
  }
  const int segW = 56;
  const int span = w - segW;
  int t = (elapsedMs - 1200) % 1400;      // 1.4s 一个来回
  if (t > 700) t = 1400 - t;              // 折返，别一直往右跑
  int px = x + (span * t) / 700;
  if (px == s_slideX) return;             // 位置没变就不动屏
  s_slideX = px;
  lcd.fillRect(x, y, w, h, bgAt(y));
  lcd.fillRect(px, y, segW, h, C_ACCENT);
}

// ---------------------------------------------------------------- 提示页
void uiMsg(const char* big, const char* small) {
  if (!tgt) return;
  fillVGrad(tgt);
  fillHGrad(tgt, 0, 0, SCR_W, 4, C_ACCENT, C_ACCENT2);

  if (big) {
    int w = cnTextWidth(big, CN_SIZE_BIG);
    if (w <= SCR_W - 16) cnDraw(tgt, big, (SCR_W - w) / 2, 84, C_TEXT, CN_SIZE_BIG);
    else                 cnDrawWrapped(tgt, big, 8, 82, SCR_W - 16, 2, C_TEXT, CN_SIZE_BIG);
  }
  if (small) cnDrawWrapped(tgt, small, 12, 130, SCR_W - 24, 3, C_DIM, CN_SIZE_SMALL);

  present();
  Serial.printf("[ui] %s | %s\n", big ? big : "", small ? small : "");
}

void uiPortal(const char* apSsid) {
  if (!tgt) return;
  fillVGrad(tgt);
  fillHGrad(tgt, 0, 0, SCR_W, 4, C_ACCENT, C_ACCENT2);

  int w = cnTextWidth("配网模式", CN_SIZE_BIG);
  cnDraw(tgt, "配网模式", (SCR_W - w) / 2, 12, C_ACCENT, CN_SIZE_BIG);

  tgt->drawFastHLine(12, 42, SCR_W - 24, C_LINE);

  cnDraw(tgt, "1. 手机连热点", 12, 52, C_TEXT, CN_SIZE_SMALL);
  cnDrawWrapped(tgt, apSsid, 24, 74, SCR_W - 40, 2, C_WARN, CN_SIZE_SMALL);
  cnDraw(tgt, "2. 浏览器打开", 12, 112, C_TEXT, CN_SIZE_SMALL);
  cnDraw(tgt, "192.168.4.1", 24, 134, C_ACCENT, CN_SIZE_BIG);
  cnDraw(tgt, "3. 填密码并保存", 12, 166, C_TEXT, CN_SIZE_SMALL);
  cnDraw(tgt, "可顺便改电台列表", 12, 188, C_DIM, CN_SIZE_SMALL);
  cnDraw(tgt, "保存后自动重启", 12, 208, C_DIM, CN_SIZE_SMALL);

  present();
}

// ---------------------------------------------------------------- 主界面
static void drawWifiIcon(int x, int y, int rssi, bool ok) {
  uint16_t col = ok ? C_ACCENT : C_WARN;
  int level = 0;
  if (ok) {
    if      (rssi > -55) level = 4;
    else if (rssi > -67) level = 3;
    else if (rssi > -78) level = 2;
    else                 level = 1;
  }
  for (int i = 0; i < 4; ++i) {
    int h  = 4 + i * 3;
    int bx = x + i * 6;
    int by = y + 15 - h;
    if (i < level) tgt->fillRect(bx, by, 4, h, col);
    else           tgt->drawRect(bx, by, 4, h, C_LINE);
  }
}

void uiRender(const UiState& s) {
  if (!tgt) return;
  fillVGrad(tgt);

  // ---------------- 顶栏 ----------------
  tgt->fillRect(0, 0, SCR_W, 30, C_PANEL);
  tgt->drawFastHLine(0, 30, SCR_W, C_LINE);
  drawWifiIcon(10, 7, s.rssi, s.wifiOk);

  tgt->setFont(&lgfx::fonts::Font4);
  tgt->setTextColor(C_TEXT, C_PANEL);
  const char* clk = (s.clock && *s.clock) ? s.clock : "--:--";
  tgt->drawString(clk, (SCR_W - tgt->textWidth(clk)) / 2, 2);

  // ---------------- 右上角：电池电量 ----------------
  // 版式（用户要求）：数字在左、电池符号在右、整体紧凑；数字用小一号的 Font2
  // （cnfont 只有 16/20 两档，数字又是纯 ASCII，直接用 GFX 点阵字更小更省空间）。
  {
    const int BW = 18, BH = 10;
    int bx = SCR_W - 8 - BW;             // 电池贴右边缘（留 8px）
    int by = 9;

    char bb[10];
    if (s.battPct < 0) snprintf(bb, sizeof(bb), "--%%");
    else               snprintf(bb, sizeof(bb), "%d%%", s.battPct);

    uint16_t tc = C_DIM;
    if (s.battPct >= 0 && s.battPct <= 15 && !s.charging) tc = RGB565(238, 108, 96);
    tgt->setFont(&lgfx::fonts::Font2);
    tgt->setTextColor(s.charging ? C_ACCENT : tc, C_PANEL);
    int tw = tgt->textWidth(bb);
    tgt->drawString(bb, bx - 3 - tw, 5);           // 数字在图标左侧，间距 3px
    tgt->setFont(&lgfx::fonts::Font4);             // 恢复，别影响后续绘制

    // 电池：18x10 外框 + 右侧凸点 + 按电量填充（绿/黄/红），充电时画闪电
    uint16_t frame = s.charging ? C_ACCENT : C_DIM;
    tgt->drawRect(bx, by, BW, BH, frame);
    tgt->fillRect(bx + BW, by + 3, 2, BH - 6, frame);

    int innerW = BW - 4;
    int fillW  = (s.battPct < 0) ? 0 : innerW * s.battPct / 100;
    uint16_t fc = C_OK;
    if (!s.charging) {
      if (s.battPct >= 0 && s.battPct <= 15)      fc = RGB565(232, 96, 88);
      else if (s.battPct >= 0 && s.battPct <= 40) fc = C_WARN;
    }
    if (fillW > 0) tgt->fillRect(bx + 2, by + 2, fillW, BH - 4, fc);

    if (s.charging) {   // 小闪电，压在电池中间
      int cx = bx + BW / 2, cy = by + BH / 2;
      tgt->fillTriangle(cx - 2, by + 2, cx + 2, by + 2, cx - 1, cy, C_TEXT);
      tgt->fillTriangle(cx + 1, cy, cx - 3, by + BH - 2, cx + 2, by + BH - 2, C_TEXT);
    }
  }

  // ---------------- 台名（20px，整个屏幕的主角）----------------
  const char* name = (s.station && *s.station) ? s.station : "--";
  int nw = cnTextWidth(name, CN_SIZE_BIG);
  if (nw <= SCR_W - 16) cnDraw(tgt, name, (SCR_W - nw) / 2, 42, C_TEXT, CN_SIZE_BIG);
  else                  cnDrawWrapped(tgt, name, 8, 40, SCR_W - 16, 2, C_TEXT, CN_SIZE_BIG);

  // ---------------- 正在播放 ----------------
  // 没歌名的时候留白，不要塞品牌名占位 —— 看着像"歌曲叫 Dayu-radio"
  // 歌名上移到 72（3 行到 120），给 74px 的大频谱（130~204）让位
  if (s.connecting)             cnDraw(tgt, "正在连接电台...", 12, 72, C_DIM, CN_SIZE_SMALL);
  else if (s.title && *s.title) cnDrawWrapped(tgt, s.title, 12, 72, SCR_W - 24, 3, C_DIM, CN_SIZE_SMALL);

  // ---------------- 底部 ----------------
  // 台号/状态行让位给频谱（172~204），挪到音量条下面
  char ibuf[24];
  snprintf(ibuf, sizeof(ibuf), "%u/%u", (unsigned)s.idx, (unsigned)s.total);
  cnDraw(tgt, ibuf, 12, 218, C_ACCENT, CN_SIZE_SMALL);

  if (s.status && *s.status) {
    // 播放中时右侧留 24px 给"网络流畅度"圆点；连接中不留（那时不显示）
    int barsW = (!s.connecting && s.netGrade) ? 24 : 0;
    int w = cnTextWidth(s.status, CN_SIZE_SMALL);
    cnDraw(tgt, s.status, SCR_W - 12 - barsW - w, 218, s.connecting ? C_WARN : C_DIM, CN_SIZE_SMALL);

    if (barsW) {
      // 三个圆点：点亮个数 + 颜色表示流畅度（刻意做成圆点，别跟左上角的
      // WiFi 竖条撞风格 —— 一个在说"信号强不强"，一个在说"流顺不顺"）
      uint16_t on = (s.netGrade >= 3) ? C_OK
                  : (s.netGrade == 2) ? C_WARN
                                      : RGB565(232, 96, 88);
      for (int i = 0; i < 3; i++) {
        int cx = SCR_W - 12 - 20 + i * 8;
        if (i < s.netGrade) tgt->fillCircle(cx + 2, 226, 2, on);
        else                tgt->drawCircle(cx + 2, 226, 2, C_LINE);
      }
    }
  }

  // 底部那根条：连台时是进度条（框暗色、内部由 uiConnBar 逐格填亮），放音时是音量条
  tgt->drawRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, 4, C_LINE);
  if (!s.connecting && s.volMax) {
    int fill = (int)((float)s.vol / s.volMax * (BAR_W - 4));
    if (fill > 2) tgt->fillRoundRect(BAR_X + 2, BAR_Y + 2, fill, BAR_H - 4, 2, C_ACCENT);
  }

  // 底部强调线
  fillHGrad(tgt, 0, SCR_H - 4, SCR_W, 4, C_ACCENT, C_ACCENT2);

  // 整屏推送前把频谱当前帧画进 sprite（画进不推送，随本次 push 一起上屏）
  spectrumRedraw();

  present();

  // 推完屏再把进度条画上去（直写，不走 sprite）
  if (s.connecting) {
    uiConnBarReset();
    uiConnBar(s.progress);
  }
}
