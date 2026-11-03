#line 1 "/Users/dayu/WorkBuddy/2026-11-02-10-50-15/Dayu-radio/dayu_radio/battery.cpp"
// ============================================================================
//  battery.cpp —— 电池电量与充电检测
//
//  ★ 这里最大的坑：GPIO17 在 ESP32-S3 上是 ADC2_CH6，而 ADC2 与 WiFi 驱动共享。
//    ESP-IDF 文档原话：WiFi 启动后 ADC2 不可用。可是收音机必须一直连着 WiFi，
//    所以这条路只能"尽力而为"：实测大部分读数是对的，但偶发会出现 0 或离谱值。
//    策略：读 16 次取平均 → 落在合理区间（1.4~2.5V 引脚电压）才算有效 →
//    EMA 平滑 → 有效值才更新百分比，无效就沿用上次（不会因为一次坏读跳 100%→0%）。
//    板上的两个 100k 分压源阻抗 50k，远超 ADC 推荐的 10k 以内，读数会偏低一点，
//    这里按实测经验做一点补偿（BATT_CAL），换板/改电阻时只需改这一个系数。
//
//  百分比走锂电放电曲线分段线性插值（3.3V 空 ~ 4.2V 满），比线性 3.0~4.2 准得多：
//  锂电池在 3.7~3.9V 这段是平台区，线性映射会让"还有一半电"显示成 30%。
// ============================================================================
#include "battery.h"
#include "config.h"
#include <Arduino.h>

static int      s_pct      = -1;      // -1 = 未知
static bool     s_charging = false;
static uint16_t s_mv       = 0;       // 电池电压（平滑后）
static uint16_t s_pinMv    = 0;       // 引脚电压（最近一次有效读数）
static uint32_t s_lastChg  = 0;
static uint32_t s_lastAdc  = 0;

// 分压源阻抗的补偿系数。实测：引脚读数 2060mV ×2 = 4120mV，正是充电中电池的
// 合理电压（4.1~4.2V），说明 analogReadMilliVolts 的出厂校准已经把偏差吃掉了。
// 最初按"源阻抗 50k 会拉低读数"预估填了 1.06，结果反而把 4.12V 虚高成 4.37V —— 改回 1.0。
// 换板/改分压电阻后如果发现读数系统性偏低/偏高，只调这一个系数。
#define BATT_CAL 1.00f

// 锂电 OCV 曲线（单节）：电压 mV → 电量 %
static const struct { uint16_t mv; uint8_t pct; } kCurve[] = {
  {4200, 100}, {4100, 90}, {4000, 80}, {3930, 70}, {3870, 60}, {3820, 50},
  {3790, 40},  {3770, 30}, {3740, 20}, {3680, 10}, {3600, 5},  {3450, 2}, {3300, 0}
};

static int mvToPct(uint16_t mv) {
  const int N = sizeof(kCurve) / sizeof(kCurve[0]);
  if (mv >= kCurve[0].mv) return 100;
  if (mv <= kCurve[N - 1].mv) return 0;
  for (int i = 0; i < N - 1; i++) {
    if (mv <= kCurve[i].mv && mv >= kCurve[i + 1].mv) {
      int span = kCurve[i].mv - kCurve[i + 1].mv;
      int dp   = kCurve[i].pct - kCurve[i + 1].pct;
      return kCurve[i + 1].pct + (mv - kCurve[i + 1].mv) * dp / span;
    }
  }
  return 0;
}

void batteryBegin() {
  pinMode(PIN_CHG_DET, INPUT_PULLDOWN);   // 悬空稳定读低（未接充电器）
  pinMode(PIN_BATT_ADC, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATT_ADC, ADC_11db);   // 0~2.5V 量程，够 2.1V

  s_charging = (digitalRead(PIN_CHG_DET) == HIGH);
  s_lastChg  = millis();
  s_lastAdc  = millis() - BATT_SAMPLE_MS;   // 首帧就读一次
  Serial.printf("[batt] 充电检测 GPIO%d，电压采样 GPIO%d\n", PIN_CHG_DET, PIN_BATT_ADC);
}

// 读一次 ADC（16 次平均），有效才返回 true
static bool readAdcOnce(uint16_t& pinMv) {
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) acc += analogReadMilliVolts(PIN_BATT_ADC);
  uint32_t mv = acc / 16;
  // 合理区间：电池 3.0~4.4V → 引脚 1.5~2.2V
  if (mv < 1200 || mv > 2600) return false;
  pinMv = (uint16_t)mv;
  return true;
}

bool batteryTick() {
  uint32_t now = millis();
  bool changed = false;

  // ---- 充电检测：纯 digitalRead，快，500ms 一次 ----
  if (now - s_lastChg >= CHG_POLL_MS) {
    s_lastChg = now;
    bool chg = (digitalRead(PIN_CHG_DET) == HIGH);
    if (chg != s_charging) {
      s_charging = chg;
      changed = true;
      Serial.printf("[batt] %s\n", chg ? "开始充电" : "充电结束（用电池）");
    }
  }

  // ---- 电压：15s 一次，16 次平均 + EMA ----
  if (now - s_lastAdc >= BATT_SAMPLE_MS) {
    s_lastAdc = now;
    uint16_t pinMv;
    if (readAdcOnce(pinMv)) {
      s_pinMv = pinMv;
      uint16_t mv = (uint16_t)(pinMv * BATT_DIV * BATT_CAL);
      if (s_mv == 0) s_mv = mv;                     // 首次直接取
      else           s_mv = (uint16_t)((s_mv * 7 + mv * 3) / 10);   // EMA 0.3
      int pct = mvToPct(s_mv);
      if (pct != s_pct) {
        changed = true;
        Serial.printf("[batt] %umV → %d%%%s\n", s_mv, pct, s_charging ? "（充电中）" : "");
        s_pct = pct;
      }
    } else {
      // ADC2 被 WiFi 抢了/读崩了：不更新，保持上次值，别让屏幕 100%→0% 乱跳
      Serial.printf("[batt] ADC 读数异常，沿用上次 %umV / %d%%\n", s_mv, s_pct);
    }
  }

  return changed;
}

int      batteryPercent()      { return s_pct; }
bool     batteryCharging()     { return s_charging; }
uint16_t batteryMilliVolts()   { return s_mv; }
uint16_t batteryPinMilliVolts(){ return s_pinMv; }
