#line 1 "/Users/dayu/WorkBuddy/2026-11-02-10-50-15/Dayu-radio/dayu_radio/spectrum.h"
// ============================================================================
//  spectrum.h —— 播放频谱律动（多风格，网页设置页可切换）
//  数据链：Audio 库的 weak 钩子 audio_process_i2s（音量/增益之后的 48kHz 输出）
//          → 环形 PCM 缓冲 → 主循环 256 点 FFT → 24 个对数频带 → sprite 绘制
//  静音时（音量坡道/按下换台）增益为 0，PCM 全零，图形自然落空 —— 和耳朵一致。
// ============================================================================
#pragma once

// 频谱风格（网页设置页下拉的顺序，别随意改：NVS 里存的是编号）
enum SpecStyle {
  SPEC_HEAT    = 0,   // 热力柱：蓝→青→绿→黄（按高度着色）+ 橙色峰值点
  SPEC_MIRROR  = 1,   // 镜像双翼：低频居中、向两侧对称展开
  SPEC_WAVE    = 2,   // 声纹波形：时域 PCM 示波器，自动增益
  SPEC_LED     = 3,   // LED 点阵：离散方格的电平表，暗格打底
  SPEC_RAINBOW = 4,   // 彩虹柱：按频率横向着色，柱顶白色高光
  SPEC_COUNT
};

// FFT 与窗函数初始化（setup 里调一次）
void spectrumBegin();

// 主循环每圈调用。内部 80ms 节流；音频数据不流动时自动画成空白。
void spectrumTick();

// 整屏重绘（uiRender）之前把当前帧画进 sprite。
void spectrumRedraw();

// 切换风格（网页 /spec 与开机恢复共用）：清状态、立即重画，保存即生效不重启。
// repaint=false 用于 setup 早期恢复配置：那时屏幕上是开机画面，不能往上推频谱块。
void spectrumSetStyle(int style, bool repaint = true);
int  spectrumGetStyle();

const char* spectrumStyleName(int style);
