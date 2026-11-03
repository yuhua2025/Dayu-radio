#line 1 "/Users/dayu/WorkBuddy/2026-11-02-10-50-15/Dayu-radio/dayu_radio/ui.h"
// ============================================================================
//  ui.h —— 屏幕驱动（LovyanGFX + ST7789）与界面绘制
// ============================================================================
#pragma once

#include <LovyanGFX.hpp>
#include "config.h"

// ---------------------------------------------------------------- 屏幕
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;
      cfg.spi_mode   = 0;
      // ★ 27MHz（原 40MHz）：整屏 sprite 在 PSRAM 上，pushSprite 走 DMA 从 PSRAM 读，
      //   40MHz 下偶发传输不完整 —— 现象就是"开机画面卡半屏"：上半屏是新画面、
      //   下半屏还停在上一帧。降到 27MHz 换取稳定，代价只是整屏刷新 30ms→45ms，
      //   对这个界面（400ms 重绘一次、频谱 80ms 局部推送）完全无感。
      cfg.freq_write = 27000000;
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = false;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk   = PIN_LCD_SCL;
      cfg.pin_mosi   = PIN_LCD_SDA;
      cfg.pin_miso   = -1;
      cfg.pin_dc     = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs           = PIN_LCD_CS;
      cfg.pin_rst          = PIN_LCD_RST;
      cfg.pin_busy         = -1;
      cfg.panel_width      = SCR_W;
      cfg.panel_height     = SCR_H;
      cfg.memory_width     = SCR_W;
      cfg.memory_height    = SCR_H;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.readable         = false;
      cfg.invert           = LCD_INVERT;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = PIN_LCD_BL;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

extern LGFX lcd;

// ---------------------------------------------------------------- 界面状态
struct UiState {
  const char* station;   // 台名
  const char* title;     // 正在播放（ICY 流标题）
  const char* status;    // 状态文字
  uint16_t    idx;       // 第几个（1 起）
  uint16_t    total;
  uint8_t     vol;
  uint8_t     volMax;
  int         rssi;      // dBm
  bool        wifiOk;
  const char* clock;     // "12:34"
  bool        connecting;// true = 正在连台：底部那根条改成进度条
  uint16_t    progress;  // 连接进度 0..1000
  int8_t      battPct;   // 电量 0..100；-1 = 未知（右上角电池图标）
  bool        charging;  // 充电中（电池图标里画闪电）
  uint8_t     netGrade;  // 网络流畅度 0 不显示 / 1 卡 / 2 一般 / 3 流畅（右下角三圆点）
};

void uiBegin();

// 背景渐变在 y 处的颜色（频谱等直写小块擦除用，写在 ui.cpp 里的原因是渐变插值在那边）
uint16_t uiBgAt(int y);

// ---- 频谱专用（spectrum.cpp）----
// 频谱画两份：一份进整屏 sprite（整屏刷新自动带上、串口抓屏也看得到），
// 一份进频谱自己的小 sprite 推上屏（80ms 一帧的局部刷新，~1ms，不整屏闪）。
lgfx::LGFX_Sprite& uiSpriteGet();

// 开机画面上的"当前在干什么"。status 为 null 表示只更新进度条。
void uiBootStep(const char* status, int permille);

void uiMsg(const char* big, const char* small);                                  // 全屏提示
void uiPortal(const char* apSsid);                                               // 配网提示
void uiRender(const UiState& s);                                                 // 主界面

// 换台时的进度条（0..1000）。只直写音量条那一小块，不整屏重绘，单次约 1ms。
// 换台全程界面**不跳页**：就在主界面上把台名换掉、把这条填起来。
void uiConnBar(int permille);
void uiConnBarReset();

// 连台等待中的"还活着"反馈：右上角状态改成"连接中 N.Ns"，底部条从进度条
// 变成来回滑动的指示条。只重画两小块，不整屏重绘（整屏要 33ms，这个约 2ms）。
// 之前等待期间界面完全静止，看着像死机 —— 这个函数就是解决"卡住"的观感。
void uiWaitTick(int elapsedMs);

// 【临时诊断】把离屏缓冲里的画面按 RGB565 原样吐到串口，用来远程核版式
void uiDump(Stream& out);
