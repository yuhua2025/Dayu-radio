// ============================================================================
//  cnfont.cpp —— 中文点阵字渲染（多字号）
//
//  字模格式：每个字定长，按行存，行内高位在左（MSB first），1bpp。
//    rowBytes = ceil(size / 8)   → 16px 是 2 字节/行，20px 是 3 字节/行
//    glyphBytes = rowBytes * size
//
//  一个字：按行把连续的点合并成一条 drawFastHLine，比逐像素快一个量级。
// ============================================================================
#include "cnfont.h"
#include <Arduino.h>

// ---------------------------------------------------------------- 字号表
struct CnFace {
  const uint16_t* cps;
  const uint8_t*  adv;
  const uint8_t*  bits;
  uint16_t        count;
  uint8_t         size;      // 字高（= 字宽格子）
  uint8_t         rowBytes;
  uint8_t         lineH;     // 推荐行距
};

static const CnFace kFaces[] = {
  { kCn16_cps, kCn16_adv, kCn16_bits, (uint16_t)kCn16_count, 16, 2, 19 },
  { kCn20_cps, kCn20_adv, kCn20_bits, (uint16_t)kCn20_count, 20, 3, 24 },
};
static const int kFaceCount = (int)(sizeof(kFaces) / sizeof(kFaces[0]));

// 取最接近请求字号的一档（永不返回空）
static const CnFace* faceFor(int size) {
  int best = 0, bestD = 99;
  for (int i = 0; i < kFaceCount; ++i) {
    int d = (int)kFaces[i].size - size;
    if (d < 0) d = -d;
    if (d < bestD) { bestD = d; best = i; }
  }
  return &kFaces[best];
}

int cnLineHeight(int size) { return faceFor(size)->lineH; }

// ---------------------------------------------------------------- UTF-8
static inline int u8Len(uint8_t c) {
  if (c < 0x80)           return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

static inline uint16_t u8Decode(const char* p, int n) {
  switch (n) {
    case 1: return (uint8_t)p[0];
    case 2: return ((uint16_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    case 3: return ((uint16_t)(p[0] & 0x0F) << 12) | ((uint16_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    case 4: return ((uint16_t)(p[0] & 0x07) << 12) | ((uint16_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    default: return 0x3F;
  }
}

// ---------------------------------------------------------------- 查表
// 找不到返回 -1（不会越界）
static int cnIndex(const CnFace* f, uint16_t cp) {
  unsigned lo = 0, hi = f->count;
  while (lo < hi) {
    unsigned mid = (lo + hi) >> 1;
    uint16_t v   = f->cps[mid];
    if (v == cp) return (int)mid;
    if (v < cp)  lo = mid + 1;
    else         hi = mid;
  }
  return -1;
}

// 返回该字符的前进宽度
static inline int cnAdvance(const CnFace* f, int idx) {
  if (idx >= 0) return f->adv[idx];
  return f->size / 2;       // 未知字符按半个字宽占位
}

int cnTextWidth(const char* utf8, int size) {
  if (!utf8) return 0;
  const CnFace* f = faceFor(size);
  int w = 0;
  const char* p = utf8;
  while (*p) {
    int n = u8Len((uint8_t)*p);
    uint16_t cp = u8Decode(p, n);
    w += cnAdvance(f, cnIndex(f, cp));
    p += n;
  }
  return w;
}

// ---------------------------------------------------------------- 绘制
static void drawGlyph(lgfx::LovyanGFX* g, const CnFace* f,
                      int x, int y, int idx, int adv, uint16_t fg) {
  const uint8_t* b = f->bits + (unsigned)idx * (unsigned)(f->rowBytes * f->size);
  const int sh = 32 - f->rowBytes * 8;      // 让行内首 bit 对齐到 bit31
  for (int yy = 0; yy < f->size; ++yy) {
    const uint8_t* rp = b + yy * f->rowBytes;
    uint32_t row = 0;
    for (int k = 0; k < f->rowBytes; ++k) row = (row << 8) | rp[k];
    row <<= sh;
    if (!row) continue;
    int xx = 0;
    while (xx < adv) {
      if (row & (0x80000000u >> xx)) {
        int s = xx;
        while (xx < adv && (row & (0x80000000u >> xx))) ++xx;
        g->drawFastHLine(x + s, y + yy, xx - s, fg);
      } else {
        ++xx;
      }
    }
  }
}

// 未知字符：画一个方框，一眼能看出缺字
static void drawTofu(lgfx::LovyanGFX* g, const CnFace* f,
                     int x, int y, int adv, uint16_t fg) {
  int w = adv > 4 ? adv - 2 : 4;
  g->drawRect(x, y + 1, w, f->size - 4, fg);
}

void cnDraw(lgfx::LovyanGFX* g, const char* utf8, int x, int y, uint16_t fg, int size) {
  if (!g || !utf8) return;
  const CnFace* f = faceFor(size);
  g->startWrite();
  const char* p = utf8;
  while (*p) {
    int n  = u8Len((uint8_t)*p);
    uint16_t cp = u8Decode(p, n);
    int idx = cnIndex(f, cp);
    int adv = cnAdvance(f, idx);
    if (idx >= 0) drawGlyph(g, f, x, y, idx, adv, fg);
    else          drawTofu (g, f, x, y, adv, fg);
    x += adv;
    p += n;
  }
  g->endWrite();
}

int cnDrawWrapped(lgfx::LovyanGFX* g, const char* utf8, int x, int y,
                  int maxW, int maxLines, uint16_t fg, int size) {
  if (!g || !utf8 || !*utf8) return 0;
  const CnFace* f = faceFor(size);
  const char* p = utf8;
  int line = 0, cx = x;

  g->startWrite();
  while (*p && line < maxLines) {
    int n  = u8Len((uint8_t)*p);
    uint16_t cp = u8Decode(p, n);
    int idx = cnIndex(f, cp);
    int adv = cnAdvance(f, idx);

    if (cx + adv > x + maxW && cx > x) {   // 换行
      ++line;
      if (line >= maxLines) break;
      cx = x;
      y += f->lineH;
    }
    if (idx >= 0) drawGlyph(g, f, cx, y, idx, adv, fg);
    else          drawTofu (g, f, cx, y, adv, fg);

    cx += adv;
    p  += n;
  }
  g->endWrite();
  return line + 1;
}
