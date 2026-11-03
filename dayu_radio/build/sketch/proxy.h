#line 1 "/Users/dayu/WorkBuddy/2026-11-02-10-50-15/Dayu-radio/dayu_radio/proxy.h"
// ============================================================================
//  proxy.h —— 本机中继代理（"预取暖机"）
//
//  【为什么需要它】
//  蜻蜓FM 每路流在 HTTP 头之后要停 150~1650ms 才吐第一个音频字节（实测串口
//  时间戳：[audio +194ms] ~ [audio +1669ms] 才出现 chunked data transfer）。
//  这段完全由服务器决定，客户端怎么优化都没用，用户看到的就是"按一下要等好几秒"。
//
//  【做法】
//  放当前台的时候，后台先把"下一个台"的上游连接建好、响应头和头几 KB 音频抢下来
//  存在 PSRAM 里。用户按键时，audio 连的是本机 127.0.0.1:18080，代理立刻把攒好的
//  字节吐出去 —— 服务器那段暖机时间已经在放歌的时候烧掉了。
//
//  【保底】
//  没预取到（开机第一个台 / 预取失败 / 预取过期）时，代理自己现连上游，行为跟
//  以前完全一样。所以这个模块只会更快，不会更慢。
// ============================================================================
#pragma once

#include <Arduino.h>

// 起监听 + 后台任务。返回 false 表示端口起不来，调用方应退回直连上游。
bool proxyBegin();

// 这个台现在能不能"立刻、且不卡界面"地走代理
// （= 响应头已经完整拿到，并且已经有 body 字节）。
// 满足这一条时 audio.connecttohost() 进来是毫秒级的：代理手里有现成的头。
bool proxyCanServe(uint16_t idx);

// 响应头是否已经拿到（body 多少都行）。
// warmWait 等的是这个，不是"攒够 18KB"—— 见 proxy.cpp 顶部【为什么阈值要分层】。
bool proxyHasHeader(uint16_t idx);

// 预取状态快照，给 warmWait 判断"继续等 / 放弃 / 换个台"。
struct ProxyWarm {
  int8_t   state;   // -1 = 没在暖这个台, 0 = 待连(或连不上在重试), 1 = 等响应头, 2 = 收body, 3 = 攒够
  uint32_t bytes;   // 已攒 body 字节
  uint8_t  fails;   // 这个台连续连接失败次数（>=2 基本可以判死）
};
ProxyWarm proxyWarm(uint16_t idx);

// audio 该连的地址：http://127.0.0.1:18080/<idx>
String proxyUrlFor(uint16_t idx);

// 告诉代理"刚刚换成 cur 了，请去暖 next"（传 0xFFFF = 不用暖）。
// cur 必须传：代理只用一个槽位，被跳过没播过的台必须立刻作废，
// 否则它会一直占着槽位到 45 秒过期，后面的台全都暖不上。
void proxySetNext(uint16_t cur, uint16_t next);

// 代理是否可用
bool proxyReady();

// 网络流畅度原始指标：距上次从上游读到音频数据的毫秒数。
// 返回 0 表示当前没有在转发（换台中 / 未播放），调用方应视为"未知"。
// 这是"会不会卡"最直接的信号：CDN 供数间隔一拉长，解码器存粮就在见底。
uint16_t proxyFeedGapMs();
