// ============================================================================
//  buttons.h —— 按键
//
//  【最终结论：全部用轮询，不要用中断】
//  这里踩过最大的坑就是"主按键用 GPIO 中断"。当初的理由是
//  "connecttohost() 会阻塞 300~1400ms，轮询会漏掉按键"——这个理由现在不成立了：
//  换台改成"先预热、再连接"之后，阻塞那段已经搬进我们自己的可轮询循环，
//  主循环每 2~5ms 就转一圈。
//
//  而中断方案引入的两个判据，对真按键是灾难性的（对串口模拟按键却永远成立，
//  所以测试时看不出来 —— 这就是"串口丝滑、实体键废掉"的由来）：
//    · "必须出现 ≥80ms 稳定高电平才认下一次按下" —— 松手弹跳不停刷新高电平起点
//    · 在中断里 digitalRead() 判方向 —— 中断延迟后电平可能已回到 HIGH
//
//  轮询不看边沿，只看"电平稳定了多久"，机械弹跳在时间窗里自动消失。
//
//  【短按在"按下"那一刻就触发】
//  等松手才判定的话，"按住多久 = 响应延迟多久"。现在电平稳定 LOW 就立刻算一次，
//  跟按多久无关。长按另算，但**判据是"实际采到的连续按住时长"，不是墙钟差** ——
//  见下面【为什么不给长按用 arm(now) + 墙钟】。
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"

// ---------------------------------------------------------------- 电平
inline bool btnLevel(uint8_t pin) {
#if BTN_ACTIVE_LOW
  return digitalRead(pin) == LOW;
#else
  return digitalRead(pin) == HIGH;
#endif
}

// ---------------------------------------------------------------- 轮询式消抖
// 每轮喂一次 now，返回稳定后的"是否按下"。
//
// 【为什么主按键也用它，而不用中断】
// 曾经主按键走 GPIO 中断，判据是"必须出现一段 ≥80ms 的稳定高电平才认下一次按下"，
// 还在中断里 digitalRead() 判方向。这套判据对**串口模拟按键**永远成立（那是用
// pinMode(OUTPUT) 硬驱动电平，边沿干净单调、低电平精确保持 120ms），
// 但对真按键是灾难：
//   · 松手时触点弹跳会不停刷新"高电平起点"，真实按下被当成抖动丢掉
//   · 中断延迟后读到电平可能已经回到 HIGH，一次按下被判成"上升沿"
// 结果就是"串口换台丝滑，实体键按了没反应"。
//
// 轮询根本不看边沿，只看"电平稳定了多久"，机械弹跳在时间窗里自动消失。
// 采样间隔现在只有 2~5ms（换台也不再阻塞主循环），够用了。
struct BtnPoll {
  uint8_t  pin       = 0xFF;
  uint16_t debounceMs = BTN_DEBOUNCE_MS;  // 电平要稳定这么久才认
  bool     raw       = false;   // 上次采样到的原始电平
  bool     stable    = false;   // 消抖后的电平
  uint32_t changedAt = 0;       // 原始电平上次变化的时刻
  uint16_t flips     = 0;       // 原始电平翻转次数（诊断：一次按压正常 2~6 次）
  uint32_t lastFlipAt = 0;

  void begin(uint8_t p, uint16_t ms = BTN_DEBOUNCE_MS) {
    pin = p;
    debounceMs = ms ? ms : BTN_DEBOUNCE_MS;
    pinMode(p, INPUT_PULLUP);
    raw = stable = btnLevel(p);
    changedAt = millis();
    flips = 0;
  }

  // 硬件被"接管"过（比如串口模拟按键把脚设成输出）之后复位消抖状态，
  // 否则 stable 会停在旧值，第一次真实按压被吞掉。
  void resync(uint32_t now) {
    raw = stable = btnLevel(pin);
    changedAt = now;
  }

  bool read(uint32_t now) {
    if (pin == 0xFF) return false;
    bool r = btnLevel(pin);
    if (r != raw) { raw = r; changedAt = now; ++flips; lastFlipAt = now; }
    if ((uint32_t)(now - changedAt) >= (uint32_t)debounceMs) stable = r;
    return stable;
  }
};

// ---------------------------------------------------------------- 长按
// 【为什么不给长按用 arm(now) + 墙钟】
// 老实现是：按下时 arm(now)，之后每轮 poll(电平, now) 里判 now - startAt >= 5s。
// 它有两个致命前提，真机上一条都保不住：
//   · "每轮都会 poll" —— 主循环是会卡住的（audio.connecttohost 的忙等、
//     连续跳死台最多十几秒）。一卡，poll 只被调用一次，拿墙钟一减就够 5 秒。
//   · "stillDown 是当下的电平" —— g_btnHeld 只在上一次采样时更新过。
//     触点若有连续抖动，BtnPoll 的 changedAt 会一直刷新，stable 就**卡在 true**，
//     g_btnHeld 永远不回落。
// 两条一叠加，就是用户遇到的那次："我按了一下，怎么变成清除配网设置了"。
//
// 现在改成**只累计真正采到"按下"的那段时间**，而且每个采样间隔最多算 200ms：
// 主循环卡住的那段等于不计时。少计时只是让人多按一会儿，多计时会删掉 WiFi 密码。
// 实现直接在 dayu_radio.ino 的 pollBootButton() 里，这里不需要结构体了。
