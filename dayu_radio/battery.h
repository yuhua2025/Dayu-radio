// ============================================================================
//  battery.h —— 电池电量与充电检测
//  硬件：GPIO17 = 电池电压（3.7V 锂电经 100k+100k 分压，1/2）
//        GPIO38 = 充电检测（高电平 = 充电中）
// ============================================================================
#pragma once
#include <stdint.h>

void batteryBegin();

// 主循环调用：内部按周期轮询（充电 500ms / 电压 15s）。
// 返回 true = 电量百分比或充电状态发生了变化（调用方可以立刻刷屏）。
bool batteryTick();

int      batteryPercent();      // 0..100；-1 = 还没读到有效值
bool     batteryCharging();     // 充电脚高电平
uint16_t batteryMilliVolts();   // 电池端电压（已按分压比还原），0 = 未知
uint16_t batteryPinMilliVolts();// 引脚端原始电压（诊断用，判断 ADC 是否正常）
