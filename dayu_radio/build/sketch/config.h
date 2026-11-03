#line 1 "/Users/dayu/WorkBuddy/2026-11-02-10-50-15/Dayu-radio/dayu_radio/config.h"
// ============================================================================
//  config.h —— Dayu-radio（ESP32-S3 / 16MB flash / 8MB OPI PSRAM / ST7789 240²）
//  硬件是星智 Cube 1.54 WiFi，引脚来自 78/xiaozhi-esp32 的
//  nologo/xingzhi-cube-1.54tft-wifi 板级配置
// ============================================================================
#pragma once

// ---------- 品牌 ----------
#define BRAND_NAME      "Dayu-radio"
#define BRAND_NAME_CN   "网络收音机"
#define FW_VERSION      "3.0"     // 3.0：配网与网页设置整套换成自实现（见 wifi_web.h）

// ---------- 屏幕 ST7789 240x240 (SPI) ----------
#define PIN_LCD_SDA   10   // MOSI
#define PIN_LCD_SCL   9
#define PIN_LCD_DC    8
#define PIN_LCD_CS    14
#define PIN_LCD_RST   18
#define PIN_LCD_BL    13   // 背光

#define SCR_W         240
#define SCR_H         240

// 若开机黑屏但背光亮：把 LCD_INVERT 取反
#define LCD_INVERT    true
// 若画面上下颠倒：把 LCD_ROTATION 改成 2（或 0/1/3 试）
#define LCD_ROTATION  0

// ---------- 喇叭 I2S ----------
#define PIN_I2S_BCLK  15
#define PIN_I2S_LRC   16
#define PIN_I2S_DOUT  7

// ---------- 按键（按下 = 接地） ----------
#define PIN_BTN_BOOT  0    // 按下立刻：下一个台   一直按住 5s：重新配网
#define PIN_BTN_VOLUP 40   // 音量 +
#define PIN_BTN_VOLDN 39   // 音量 -

// ---------- 电源检测 ----------
// 充电检测：高电平 = 充电中。用下拉输入，没接充电器/悬空时稳定读低。
#define PIN_CHG_DET   38
// 电池电压：3.7V 锂电正极经两个 100k 分压（1/2）到 GPIO17。
// ★ GPIO17 在 ESP32-S3 上是 ADC2_CH6 —— ADC2 与 WiFi 驱动共享，
//   官方说 WiFi 启动后 ADC2 不可用。实测这条线上 analogReadMilliVolts 有值，
//   但仍按"不可靠"对待：读到 0 或明显越界就沿用上次有效值（见 battery.cpp）。
#define PIN_BATT_ADC  17
#define BATT_DIV      2.0f    // 分压比 1:1 → 引脚电压 ×2 = 电池电压
#define BATT_SAMPLE_MS 15000  // 电压采样周期（ADC 别频繁读，WiFi 在跑）
#define CHG_POLL_MS    500    // 充电脚轮询周期

#define BTN_ACTIVE_LOW      1
// 消抖：**电平**要稳定这么久才认（轮询采样，不看边沿）。
// 以前这里是"两个下降沿间隔"的时间窗，配一条"必须出现 80ms 稳定高电平"的额外判据，
// 走 GPIO 中断。那套对串口模拟按键永远成立（电平是硬驱动的），对真按键却会整批
// 吞掉按压 —— 松手弹跳不停刷新"高电平起点"，中断延迟后读到的电平又可能已恢复。
// 现在统一改成轮询：机械弹跳在时间窗里自动消失，不需要任何边沿判据。
#define BTN_DEBOUNCE_MS     50   // 音量 ± 用（按住还要连发，宁可稳一点）
#define BTN_MAIN_DEBOUNCE_MS 25  // 主按键用：滤得掉 5~15ms 弹跳，又不拖慢 60~80ms 的快按
// 长按（清配网）阈值。故意设得很长，而且判据是"**实际采到的**连续按住时长"
// （见 buttons.h 底部说明）—— 这个动作会删掉 WiFi 密码，绝不能靠运气不被误触。
// 曾经 5000ms + "按下时刻到现在的墙钟差"，一次普通按压就把配网清了。
#define BTN_LONGPRESS_MS  8000
#define BTN_HOLD_SAMPLE_CAP 200  // 每个采样间隔最多计入这么多毫秒（主循环被卡住时不计时）
#define BTN_QUEUE_MAX        8   // 最多记几次待处理的按下（超出合并，防失控）

// ---------- 音量 ----------
#define VOLUME_STEPS  21
#define VOLUME_DEFAULT 12

// ---------- 电台表容量 ----------
// 原厂 114 台里 62 个可用 + 9 个公共流 = 71，再给自定义台留足余量。
// 每台约 100 字节堆（String），128 个约 13KB，8MB PSRAM 完全无压力。
#define MAX_STATIONS  128

// ---------- 换台速度（见 README §八）----------
// 换台"从按下按键到出声"分三段：
//   ① 屏幕刷新 ~31ms（把"正在连接 XXX"推上去）
//   ② TCP 连接  ~30~90ms
//   ③ 等服务器发数据 + 解码  250~1000ms  ← 占 80%，且由电台服务器决定
// 第三段是蜻蜓FM 每个频道开播前自己攒缓冲（用电脑直连实测中位数 530~700ms），
// 改客户端代码压不下去。下面三个参数只能压①②和"发现数据到了"的反应延迟。
#define SWITCH_WARM_MS     4000   // "抢跑"窗口：换台后这段时间多喂几次 audio.loop()
#define SWITCH_PUMP_LOOPS  10     // 抢跑期间每次主循环调几次 audio.loop()（实测 10 比 3 快约 80ms）
#define SWITCH_SPIN_DELAY  0      // 抢跑期间主循环延时（ms）；0 = 不停顿地吃数据
#define SWITCH_GIVEUP_MS   9000   // 连上了但一直没声音，超过这么久就认为这个台死了，自动跳下一个
#define SWITCH_RETRY_MS     700   // TCP 都没连上时，隔多久重试一次
#define SWITCH_MAX_TRIES      2   // 同一个台最多连几次，还不行就跳下一个
#define SWITCH_MAX_SKIPS     12   // 连续跳过这么多个台都失败，说明是网络问题，别再刷台了

// ---------- 预取代理（见 proxy.h）----------
// 1 = 音频流走本机 127.0.0.1 代理，放当前台时提前把下一个台暖好，
//     换台就能跳过服务器 150~1650ms 的预热。
// 0 = 直接连上游（老行为）。出问题想快速排除时可以改 0 重编。
#define USE_PREFETCH_PROXY 1

// ---------- 配网（见 wifi_web.h：自实现 AP + DNS 捕获门户 + 中文配网页）----------
#define AP_NAME       "DayuRadio"        // 热点名
#define WIFI_CONNECT_TIMEOUT_MS 15000    // 开机连 WiFi 的等待上限
#define AP_IDLE_CLOSE_MS   600000UL      // 手动开的配网热点，闲置这么久自动关（10 分钟）
// 配网成功后热点再留这么久。原来 60 秒 —— 用户连上 WiFi 后要干等一分钟才看到画面
// 变化、热点才关，感觉就是"卡住了"。8 秒足够手机页面显示"连接成功"，设备这边
// 已经在开播了，热点没有继续开着的必要。
#define AP_AFTER_CONNECT_MS 8000UL

// ---------- 配色（深靛蓝，RGB565）----------
// 背景不再是近黑（RGB 10,14,20 在 IPS 上就是一块死黑，看着像没点亮）。
// 改成"上深靛蓝 → 下近夜蓝"的竖向渐变，暗处仍有层次，也不刺眼。
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define C_BG_TOP  RGB565( 26,  38,  68)   // 渐变上端：深靛蓝
#define C_BG_BOT  RGB565(  9,  14,  30)   // 渐变下端：夜蓝
#define C_BG      RGB565( 18,  25,  48)   // 中值，兜底 / 小面积擦除

#define C_PANEL   RGB565( 36,  50,  84)   // 顶栏、卡片
#define C_PANEL_HI RGB565( 48,  66, 108)  // 进度条槽等"凹槽"
#define C_TEXT    RGB565(240, 245, 255)
#define C_DIM     RGB565(146, 162, 190)
#define C_ACCENT  RGB565( 64, 214, 200)   // 品牌青
#define C_ACCENT2 RGB565(120, 168, 255)   // 辅助冷蓝（渐变装饰）
#define C_WARN    RGB565(255, 176,  76)
#define C_OK      RGB565( 88, 210, 138)
#define C_LINE    RGB565( 54,  72, 112)   // 分隔线
