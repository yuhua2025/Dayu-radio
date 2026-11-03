// ============================================================================
//  cnfont.h —— 自绘中文点阵字（1bpp，GB2312 全字集 + ASCII，多字号）
//
//  为什么不用 u8g2 字体：
//      u8g2_font_wqy16_t_gb2312 是 GB2312 编码，没有 Unicode 索引表；
//      而 lgfx::U8g2font::getGlyph() 只认 Unicode 编码字体，
//      拿它查汉字会在字库里无界扫描 → 死循环 → 整机卡死（表现为黑屏）。
//      这里改成自己的 1bpp 位图 + 二分查找，查不到就画方框，
//      绝不会越界，也绝不会死循环。
//
//  字号：
//      16 —— 辅助文字（歌曲名、配网说明、顶部小标）
//      20 —— 台名、开机大字
//      想再加字号：改 tools/gen_cnfont.py 末尾的元组重新生成，
//      再到 cnfont.cpp 的 kFaces[] 里补一行即可，其余代码不用动。
//
//  注意：cnfont16.c / cnfont20.c 是纯 C 文件，本头文件对 C 只暴露数据数组，
//        C++ 部分（LovyanGFX 相关）全部在 __cplusplus 里。
// ============================================================================
#pragma once

#include <stdint.h>

#define CN_SIZE_SMALL  16
#define CN_SIZE_BIG    20

#ifdef __cplusplus
extern "C" {
#endif

// 每套字模都是一组平行数组（按 Unicode 升序，供二分查找）
#define CN_DECLARE(NAME)                       \
  extern const uint16_t NAME##_cps[];          \
  extern const uint8_t  NAME##_adv[];          \
  extern const uint8_t  NAME##_bits[];         \
  extern const unsigned NAME##_count

CN_DECLARE(kCn16);
CN_DECLARE(kCn20);

#undef CN_DECLARE

#ifdef __cplusplus
}   // extern "C"

#include <LovyanGFX.hpp>

// 文本像素宽度。size 取不到时自动用最接近的一档。
int  cnTextWidth(const char* utf8, int size = CN_SIZE_SMALL);

// 在 (x,y) 处绘制，y 为字顶
void cnDraw(lgfx::LovyanGFX* g, const char* utf8, int x, int y, uint16_t fg,
            int size = CN_SIZE_SMALL);

// 自动换行绘制，返回占用行数
int  cnDrawWrapped(lgfx::LovyanGFX* g, const char* utf8, int x, int y,
                   int maxW, int maxLines, uint16_t fg,
                   int size = CN_SIZE_SMALL);

// 某字号的推荐行距（16→19，20→24）
int  cnLineHeight(int size = CN_SIZE_SMALL);
#endif
