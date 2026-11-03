// ============================================================================
//  spectrum.cpp —— 播放频谱律动
//
//  数据从哪来：
//    ESP32-audioI2S 库在写 I2S 之前会调 audio_process_i2s()（weak 空函数，
//    注释写明 "after volume, gain and equalizer"）。在这里定义一个同名强符号
//    即可拿到**实际送进喇叭的 PCM**——音量坡道、静音全都如实反映，
//    而且完全不用改库文件（README「依赖库补丁」不用加条目）。
//    样本是 int32 立体声帧，有效位在高位（库里 VU 用 >>20），>>16 即 int16 域。
//    固定 48kHz 输出补丁生效后，走到这里的样本恒为 48kHz。
//
//  注意上下文：这个钩子跑在库的音频任务（核 0）里，这里只做最便宜的事——
//    混单声道 + 写环形缓冲，绝不碰屏幕、绝不 malloc。
//
//  绘制跑在主循环：80ms 一帧，256 点 FFT，24 个对数频带。
//  柱子上升快、下降慢，峰值点悬停后缓降 —— 经典频谱观感。
// ============================================================================
#include "spectrum.h"
#include "ui.h"
#include "config.h"
#include <LovyanGFX.hpp>
#include <Arduino.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------- PCM 采集
#define PCM_N 1024                       // 环形缓冲：48kHz × ~21ms
static int16_t          s_pcm[PCM_N];
static volatile uint32_t s_head = 0;     // 已写入总数（单调递增，取模访问）

// ★ 出声检测（主循环用它判断"到底有没有在响"，判据见 spectrum.h）
//   s_written ：累计送进 I2S 的样本数。只要解码器在出数据它就在涨 —— 音量 0 时
//              送的是全零样本，它照样涨，所以"有没有数据"和"音量多大"是两件事。
//   s_audibleAt：最近一次出现"非数字静音"样本的时刻。数字静音（全零）是精确的 0，
//              所以一个极低的阈值就能把"真静音"和"很轻的音乐"分开。
static volatile uint32_t s_written   = 0;
static volatile uint32_t s_audibleAt = 0;
#define AUDIBLE_PEAK 24                  // int16 域约 -62dBFS，压不住正常节目的弱音

void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S) {
  *continueI2S = true;                   // 与库默认行为一致：继续送 I2S
  uint32_t h = s_head;
  int32_t  peak = 0;
  for (int i = 0; i < validSamples; i++) {
    // 立体声混单声道，int32 高位域 → int16 域（先加后移防溢出偏置可忽略）
    int32_t m = (outBuff[2 * i] + outBuff[2 * i + 1]) >> 17;
    if (m > 32767)  m = 32767;
    if (m < -32768) m = -32768;
    s_pcm[h++ & (PCM_N - 1)] = (int16_t)m;
    int32_t a = (m < 0) ? -m : m;
    if (a > peak) peak = a;
  }
  s_head = h;
  s_written += (uint32_t)validSamples;
  if (peak > AUDIBLE_PEAK) s_audibleAt = millis();   // 只有真的有内容才取时间，省开销
}

uint32_t spectrumSampleCount() { return s_written; }

uint32_t spectrumSilentMs() {
  uint32_t at = s_audibleAt;
  if (!at) return 0xFFFFFFFFu;
  return millis() - at;
}

// ---------------------------------------------------------------- FFT
#define FFT_N 256
static float s_win[FFT_N];               // Hann 窗
static float s_re[FFT_N], s_im[FFT_N];

// 256 点 radix-2 迭代 FFT（float）。~1ms @240MHz，80ms 一次完全无感。
static void fft256() {
  for (int i = 1, j = 0; i < FFT_N; i++) {
    int bit = FFT_N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = s_re[i]; s_re[i] = s_re[j]; s_re[j] = t;
      t = s_im[i]; s_im[i] = s_im[j]; s_im[j] = t;
    }
  }
  for (int len = 2; len <= FFT_N; len <<= 1) {
    float ang = -2.0f * PI / len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < FFT_N; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = i + k + len / 2;
        float tr = s_re[b] * cr - s_im[b] * ci;
        float ti = s_re[b] * ci + s_im[b] * cr;
        s_re[b] = s_re[a] - tr; s_im[b] = s_im[a] - ti;
        s_re[a] += tr;          s_im[a] += ti;
        float nc = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nc;
      }
    }
  }
}

static void buildBands();   // 定义在绘制段前，这里先声明

void spectrumBegin();   // 实现在文件末尾（要用到绘制段的 sprite）

// ---------------------------------------------------------------- 频带映射
// 48kHz / 256 点 → bin 宽 187.5Hz。取 bin 1..56（≈187Hz ~ 10.5kHz），
// 几何分 24 桶 —— 低频细、高频粗，跟听感对齐。
#define BARS 24
static int s_binLo[BARS + 1];

static void buildBands() {
  const float fLo = 180.0f, fHi = 10500.0f;
  for (int b = 0; b <= BARS; b++) {
    float f = fLo * powf(fHi / fLo, (float)b / BARS);
    int bin = (int)(f / (48000.0f / FFT_N));
    if (bin < 1) bin = 1;
    if (bin > FFT_N / 2 - 1) bin = FFT_N / 2 - 1;
    s_binLo[b] = bin;
  }
  // 保证每桶至少 1 个 bin 且单调递增
  for (int b = 1; b <= BARS; b++)
    if (s_binLo[b] <= s_binLo[b - 1]) s_binLo[b] = s_binLo[b - 1] + 1;
}

// ---------------------------------------------------------------- 绘制
// 位置：底部大空间（台号行让位后），全宽几乎横贯 —— 32px 高的柱子才够律动感。
// 双份绘制：s_panel（224x32 小 sprite）80ms 一帧局部推屏 —— 不整屏闪；
// 大 sprite（uiSpriteGet）在整屏刷新前同步画一遍 —— 整屏刷新自动带上频谱，
// 串口抓屏（uiDump 读大 sprite）也能看到。
static const int SP_X = 8, SP_Y = 130, SP_W = SCR_W - 16, SP_H = 74;

static lgfx::LGFX_Sprite s_panel(&lcd);
static bool s_panelOk = false;

static uint8_t s_h[BARS];                // 当前柱高 0..SP_H
static uint8_t s_peak[BARS];             // 峰值点位置
static int8_t  s_peakHold[BARS];         // 峰值悬停倒计帧
static uint32_t s_lastDraw = 0;
static uint32_t s_lastHead = 0;
static float    s_dbRef = -50.0f;        // AGC 满柱参考电平（dB）
static bool     s_paintedEmpty = false;  // 空白帧只画一次

static int      s_style = SPEC_HEAT;     // 当前风格（网页可切换，NVS 记住）
static int16_t  s_wave[FFT_N];           // 波形样式用的原始 PCM（本帧）
static float    s_waveGain = 1.0f;       // 波形自动增益

// ---- 色带 1：柱子按高度取色，从底到顶「深蓝 → 蓝 → 品牌青 → 绿 → 暖黄」----
static uint16_t s_ramp[SP_H];
// ---- 色带 2（镜像双翼用）：夜蓝 → 蓝 → 青 → 白，冷色系 ----
static uint16_t s_ramp2[SP_H];
// ---- 彩虹（按频率取色）：蓝→青→绿→黄→橙→品红 ----
static uint16_t s_rainbow[BARS];

static inline uint16_t mix565(uint16_t a, uint16_t b, float f) {
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = (int)(ar + (br - ar) * f);
  int g = (int)(ag + (bg - ag) * f);
  int l = (int)(ab + (bb - ab) * f);
  return (uint16_t)((r << 11) | (g << 5) | l);
}

static void buildRamp() {
  const uint16_t stops[5] = {
    RGB565( 16,  52, 108),   // 底：夜蓝（沉下去，不跟背景抢）
    RGB565( 30, 130, 190),   // 蓝
    RGB565( 64, 214, 200),   // 品牌青
    RGB565(120, 224, 130),   // 绿
    RGB565(252, 222,  96),   // 顶：暖黄（高电平亮起来）
  };
  const uint16_t stops2[4] = {
    RGB565( 12,  40,  95),
    RGB565( 28, 110, 190),
    RGB565( 64, 214, 200),
    RGB565(235, 250, 255),
  };
  const uint16_t rain[6] = {
    RGB565( 40,  90, 235),   // 低频：蓝
    RGB565( 30, 190, 215),   // 青
    RGB565( 70, 220, 120),   // 绿
    RGB565(250, 220,  80),   // 黄
    RGB565(255, 130,  55),   // 橙
    RGB565(225,  70, 165),   // 高频：品红
  };
  const int NS = 5;
  for (int r = 0; r < SP_H; r++) {
    float t = (float)r / (SP_H - 1) * (NS - 1);
    int i = (int)t; if (i > NS - 2) i = NS - 2;
    s_ramp[r] = mix565(stops[i], stops[i + 1], t - i);
    float u = (float)r / (SP_H - 1) * 3.0f;
    int k = (int)u; if (k > 2) k = 2;
    s_ramp2[r] = mix565(stops2[k], stops2[k + 1], u - k);
  }
  for (int b = 0; b < BARS; b++) {
    float u = (float)b / (BARS - 1) * 5.0f;
    int k = (int)u; if (k > 4) k = 4;
    s_rainbow[b] = mix565(rain[k], rain[k + 1], u - k);
  }
}

// 按比例定列边界（224 除 24 余 8 摊到各列，右侧不留空）
static inline void colBounds(int b, int ox, int& x0, int& w) {
  x0 = ox + b * SP_W / BARS;
  int x1 = ox + (b + 1) * SP_W / BARS;
  w = x1 - x0 - 1;
  if (w < 2) w = 2;
}

// 按高度逐行上色的柱（色带可换：热力 / 镜像冷色）
static void heatCol(lgfx::LGFX_Sprite& g, int x0, int w, int oy, int h,
                    const uint16_t* ramp) {
  for (int r = 0; r < h; r++)
    g.drawFastHLine(x0 + 1, oy + SP_H - 1 - r, w, ramp[SP_H - 1 - r]);
}

// 在 g 上画频谱帧。ofs=true 时以屏幕坐标画（进大 sprite），否则局部坐标（进 s_panel）
// ★ 背景色必须按**屏幕真实纵坐标**（SP_Y+y）取 —— 之前局部坐标误用了 oy+y
//   （= 屏幕顶部的亮蓝），和整屏刷新推来的正确底色每 400ms 交替 = 背景一闪一闪。
static void paintFrame(lgfx::LGFX_Sprite& g, bool ofs) {
  const int ox = ofs ? SP_X : 0, oy = ofs ? SP_Y : 0;
  for (int y = 0; y < SP_H; y++)
    g.drawFastHLine(ox, oy + y, SP_W, uiBgAt(SP_Y + y));

  int x0, w;

  switch (s_style) {

    // ① 热力柱：按高度着色，橙色峰值点（最初版，最"信息量"）
    case SPEC_HEAT:
      for (int b = 0; b < BARS; b++) {
        colBounds(b, ox, x0, w);
        heatCol(g, x0, w, oy, s_h[b], s_ramp);
        if (s_peak[b] > 2)
          g.fillRect(x0 + 1, oy + SP_H - s_peak[b], w, 1, C_WARN);
      }
      break;

    // ② 镜像双翼：12 对频段，低频在正中、高频铺向两端；冷色带 + 白色峰点
    case SPEC_MIRROR: {
      const int HALF = BARS / 2;
      for (int c = 0; c < HALF; c++) {
        int v = max(s_h[c * 2], s_h[c * 2 + 1]);
        int p = max(s_peak[c * 2], s_peak[c * 2 + 1]);
        colBounds(HALF - 1 - c, ox, x0, w);
        heatCol(g, x0, w, oy, v, s_ramp2);
        if (p > 2) g.fillRect(x0 + 1, oy + SP_H - p, w, 1, C_TEXT);
        colBounds(HALF + c, ox, x0, w);
        heatCol(g, x0, w, oy, v, s_ramp2);
        if (p > 2) g.fillRect(x0 + 1, oy + SP_H - p, w, 1, C_TEXT);
      }
      break;
    }

    // ③ 声纹波形：时域示波器。中心线上的填充 + 描线，自动增益跟随响度
    //    配色：沿时间轴横向走彩虹（复用 24 色带），填充取同色暗版 —— 比原来的
    //    单色暗青"亮"得多，也和信息量匹配（左旧右新，颜色在流动）。
    case SPEC_WAVE: {
      const int mid = oy + SP_H / 2;
      const int half = SP_H / 2 - 2;
      const uint16_t bgc = uiBgAt(SP_Y + SP_H / 2);
      for (int x = 0; x < SP_W; x += 8) g.drawPixel(ox + x, mid, C_LINE);
      for (int x = 0; x < SP_W; x++) {
        int i = x * FFT_N / SP_W;
        int a = (int)((float)s_wave[i] / 32768.0f * s_waveGain * half);
        if (a > half)  a = half;
        if (a < -half) a = -half;
        int y = mid - a;
        uint16_t col  = s_rainbow[x * BARS / SP_W];        // 按横向位置取彩虹
        uint16_t fill = mix565(col, bgc, 0.62f);           // 同色压暗做柱体
        if (a > 0)  g.drawFastVLine(ox + x, y, mid - y, fill);
        if (a < 0)  g.drawFastVLine(ox + x, mid + 1, y - mid, fill);
        g.drawPixel(ox + x, y, col);                       // 波峰亮色
        if (y > oy && y + 1 < oy + SP_H) g.drawPixel(ox + x, y + 1, mix565(col, bgc, 0.35f));
      }
      break;
    }

    // ④ LED 点阵：每柱拆成离散方格，未点亮的是暗格；峰值格用橙色
    case SPEC_LED: {
      const int CELL = 6, CELLH = 4, ROWS = SP_H / CELL;
      for (int b = 0; b < BARS; b++) {
        colBounds(b, ox, x0, w);
        int lit = (s_h[b] * ROWS + SP_H - 1) / SP_H;
        for (int r = 0; r < ROWS; r++) {
          int yy = oy + SP_H - 1 - r * CELL - (CELLH - 1);
          uint16_t col = (r < lit)
            ? s_ramp[r * (SP_H - 1) / (ROWS - 1)]     // 底格深蓝、顶格暖黄
            : RGB565(30, 40, 62);                      // 暗格：LED 没亮
          g.fillRect(x0 + 1, yy, w, CELLH, col);
        }
        int pr = s_peak[b] * ROWS / SP_H;
        if (pr >= ROWS) pr = ROWS - 1;
        if (s_peak[b] > 2)
          g.fillRect(x0 + 1, oy + SP_H - 1 - pr * CELL - (CELLH - 1), w, CELLH, C_WARN);
      }
      break;
    }

    // ⑤ 彩虹柱：颜色按频率横向渐变（与热力柱的"按高度"完全不同），柱顶白高光
    case SPEC_RAINBOW:
      for (int b = 0; b < BARS; b++) {
        colBounds(b, ox, x0, w);
        int h = s_h[b];
        if (h > 1) {
          g.fillRect(x0 + 1, oy + SP_H - h, w, h, s_rainbow[b]);
          g.fillRect(x0 + 1, oy + SP_H - h, w, 2, C_TEXT);
        }
        if (s_peak[b] > 2)
          g.fillRect(x0 + 1, oy + SP_H - s_peak[b], w, 1, C_ACCENT2);
      }
      break;
  }

  g.drawFastHLine(ox, oy + SP_H, SP_W, C_LINE);
}

static void drawEmpty() {
  paintFrame(uiSpriteGet(), true);
  if (s_panelOk) { paintFrame(s_panel, false); s_panel.pushSprite(SP_X, SP_Y); }
  s_paintedEmpty = true;
}

static void drawBars() {
  paintFrame(uiSpriteGet(), true);
  if (s_panelOk) { paintFrame(s_panel, false); s_panel.pushSprite(SP_X, SP_Y); }
  s_paintedEmpty = false;
}

void spectrumTick() {
  uint32_t now = millis();
  if (now - s_lastDraw < 80) return;     // ~12.5fps
  s_lastDraw = now;

  uint32_t head = s_head;
  bool flowing = (head != s_lastHead);   // 音频任务还在写 = 流还在
  s_lastHead = head;

  // 数据不够（刚开始播）或流断了 → 画空白
  if (!flowing || head < (uint32_t)FFT_N) {
    if (!s_paintedEmpty) drawEmpty();
    return;
  }

  // 取最新 256 个样本加窗（同时留一份原始波形给"声纹"样式）
  int wmax = 1;
  for (int i = 0; i < FFT_N; i++) {
    int16_t raw = s_pcm[(head - FFT_N + i) & (PCM_N - 1)];
    s_wave[i] = raw;
    int a = raw < 0 ? -raw : raw;
    if (a > wmax) wmax = a;
    s_re[i] = raw / 32768.0f * s_win[i];
    s_im[i] = 0.0f;
  }
  // 波形自动增益：把本帧峰值映射到近满幅，慢速跟随，别一惊一乍
  float want = 28000.0f / (float)wmax;
  if (want > 12.0f) want = 12.0f;
  if (want < 1.0f)  want = 1.0f;
  s_waveGain += 0.15f * (want - s_waveGain);
  fft256();

  // 分桶 + dB 映射
  uint8_t target[BARS];
  const float norm = 1.0f / FFT_N;
  float maxdb = -120.0f;
  float dbs[BARS];

  for (int b = 0; b < BARS; b++) {
    float mag = 0.0f;
    for (int k = s_binLo[b]; k < s_binLo[b + 1]; k++) {
      float m = sqrtf(s_re[k] * s_re[k] + s_im[k] * s_im[k]) * norm;
      if (m > mag) mag = m;
    }
    float db = 20.0f * log10f(mag + 1e-6f);
    // 高频给一点补偿（+2dB/kHz），不然中高频柱总趴着
    float f = (s_binLo[b] + 1) * (48000.0f / FFT_N);
    db += f / 1000.0f * 2.0f;
    dbs[b] = db;
    if (db > maxdb) maxdb = db;
  }

  // ★ 自动增益：音乐是宽带的，单 bin 幅度随台/节目/编码率浮动好几十 dB，
  //   固定映射窗口必然"有的台趴着、有的台顶满"。让满柱参考电平（s_dbRef）
  //   慢速跟随本帧最大值：上升快（节目一响立刻跟上，不削顶）、下降慢
  //   （安静段柱子缓缓归位，不会瞬间塌光）。实测典型范围 -90 ~ -40dB。
  if (maxdb > s_dbRef) s_dbRef += 0.25f * (maxdb - s_dbRef);
  else                 s_dbRef += 0.03f * (maxdb - s_dbRef);
  if (s_dbRef > -25.0f) s_dbRef = -25.0f;
  if (s_dbRef < -85.0f) s_dbRef = -85.0f;

  for (int b = 0; b < BARS; b++) {
    int t = (int)((dbs[b] - (s_dbRef - 42.0f)) / 42.0f * SP_H);  // 参考下 42dB 窗口
    if (t < 0) t = 0;
    if (t > SP_H) t = SP_H;
    target[b] = (uint8_t)t;
  }

  // 平滑：上升快（贴住目标），下降慢（每帧 2px）→ 律动但不闪烁
  for (int b = 0; b < BARS; b++) {
    int h = s_h[b];
    if (target[b] >= h) h = target[b];
    else                h = (h > target[b] + 2) ? h - 2 : target[b];
    s_h[b] = h;

    // 峰值：被超过就上移并悬停 ~14 帧，之后缓降
    if (h >= s_peak[b]) {
      s_peak[b] = h;
      s_peakHold[b] = 14;
    } else if (s_peakHold[b] > 0) {
      s_peakHold[b]--;
    } else if (s_peak[b] > 0) {
      s_peak[b]--;
    }
  }

  drawBars();
}

// 整屏重绘（uiRender）推 sprite 前调用：把当前帧画进 sprite（不推送，
// 随后的整屏 push 自然带上频谱），保证频谱区域不出现 400ms 的"闪没"。
void spectrumRedraw() {
  uint32_t head = s_head;
  bool flowing = (head != s_lastHead) && head >= (uint32_t)FFT_N;
  s_lastHead = head;
  if (flowing) drawBars();
  else         drawEmpty();
}

// FFT 初始化 + 频谱局部 sprite 创建（放在 setup 音频初始化之后调用）
void spectrumBegin() {
  buildBands();
  buildRamp();
  for (int i = 0; i < FFT_N; i++) {
    s_win[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (FFT_N - 1)));
  }
  s_panel.setColorDepth(16);
  s_panelOk = s_panel.createSprite(SP_W, SP_H);   // 224x74x2 ≈ 33KB，堆足够
  if (!s_panelOk) Serial.println("[spec] 局部 sprite 创建失败，只做整屏刷新");
}

// ---------------------------------------------------------------- 风格切换
static const char* const kStyleName[SPEC_COUNT] = {
  "热力柱 · 蓝绿黄", "镜像双翼 · 对称", "声纹波形 · 示波器",
  "LED 点阵 · 方格", "彩虹柱 · 横向渐变"
};

const char* spectrumStyleName(int style) {
  if (style < 0 || style >= SPEC_COUNT) return "";
  return kStyleName[style];
}

int spectrumGetStyle() { return s_style; }

void spectrumSetStyle(int style, bool repaint) {
  if (style < 0 || style >= SPEC_COUNT) return;
  if (style == s_style && s_dbRef != -50.0f) return;  // 已生效（开机初始化除外）
  s_style = style;
  memset(s_h, 0, sizeof(s_h));
  memset(s_peak, 0, sizeof(s_peak));
  memset(s_peakHold, 0, sizeof(s_peakHold));
  s_dbRef = -50.0f;
  s_waveGain = 1.0f;
  s_paintedEmpty = false;
  // ★ 开机早期恢复配置时不能重画：那时屏幕上是开机画面，drawBars() 会把
  //   频谱块直接推到屏上（s_panel.pushSprite），看起来就是"开机画面被挖掉一块/
  //   卡在半屏"。等主界面第一次 uiTick 时 spectrumRedraw 自然会画。
  if (repaint) drawBars();           // 网页切换：立刻重画，所见即所得
  Serial.printf("[spec] 风格 → %d %s\n", style, kStyleName[style]);
}
