// bc-decode-test.cpp - known-answer tests for bc-decode-web.cpp.
// em++ -O2 -std=c++17 src/bc-decode-web.cpp src/bc-decode-test.cpp -o test.js && node test.js
#include "bc-decode-web.h"
#include <cstdio>
#include <cstring>
using namespace cp2077::bc;

static int g_fail = 0;
static void Report(const char* name, bool ok) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++g_fail; }

struct BitWriter { // LSB-first, matches the BC6H/BC7 bit order
    uint8_t b[16] = {}; unsigned pos = 0;
    void Put(unsigned v, unsigned n) { for (unsigned i = 0; i < n; ++i, ++pos) if ((v >> i) & 1) b[pos >> 3] |= uint8_t(1 << (pos & 7)); }
};
static const int W3[] = { 0, 9, 18, 27, 37, 46, 55, 64 };
static const int W4[] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
static unsigned Lerp(unsigned a, unsigned b, int w) { return (a * (64 - w) + b * w + 32) >> 6; }
static unsigned Rd16(const uint8_t* p) { return p[0] | p[1] << 8; }

static void TestBC1() {
    uint8_t blk[8], out[64];
    { // solid: c0 == c1 = pure red 0xF800 -> 3-color mode; index 0/1/2 all red, index 3 transparent black
        const uint8_t b[8] = { 0x00, 0xF8, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00 }; std::memcpy(blk, b, 8);
        DecodeBlock(Kind::BC1, blk, out); bool ok = true;
        for (int i = 0; i < 16; ++i) ok &= out[i*4] == 255 && out[i*4+1] == 0 && out[i*4+2] == 0 && out[i*4+3] == 255;
        Report("BC1 solid color (c0==c1)", ok);
    }
    { // 4-color: c0 = white 0xFFFF, c1 = black 0x0000; indices 0,1,2,3 repeating
        const uint8_t b[8] = { 0xFF, 0xFF, 0x00, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 }; std::memcpy(blk, b, 8);
        DecodeBlock(Kind::BC1, blk, out); const uint8_t exp[4] = { 255, 0, 170, 85 }; bool ok = true;
        for (int i = 0; i < 16; ++i) for (int c = 0; c < 3; ++c) ok &= out[i*4+c] == exp[i & 3] && out[i*4+3] == 255;
        Report("BC1 4-color gradient", ok);
    }
    { // 3-color: c0 = black 0x0000 < c1 = 0x001F (blue 255); idx2 = (0+255)/2 = 127, idx3 transparent
        const uint8_t b[8] = { 0x00, 0x00, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 }; std::memcpy(blk, b, 8);
        DecodeBlock(Kind::BC1, blk, out);
        const uint8_t exp[4][4] = { {0,0,0,255}, {0,0,255,255}, {0,0,127,255}, {0,0,0,0} }; bool ok = true;
        for (int i = 0; i < 16; ++i) ok &= std::memcmp(out + i*4, exp[i & 3], 4) == 0;
        Report("BC1 3-color transparent", ok);
    }
    { // 565 bit replication: r=16 -> 132, g=32 -> 130, b=1 -> 8
        const unsigned c = 16 << 11 | 32 << 5 | 1; const uint8_t b[8] = { uint8_t(c), uint8_t(c >> 8), 0, 0, 0, 0, 0, 0 }; std::memcpy(blk, b, 8);
        DecodeBlock(Kind::BC1, blk, out); Report("BC1 565 expansion", out[0] == 132 && out[1] == 130 && out[2] == 8);
    }
}

static void TestBC2BC3() {
    uint8_t blk[16], out[64];
    { // BC2: alpha nibble i, color white 4-color
        for (int i = 0; i < 8; ++i) blk[i] = uint8_t((2*i) | (2*i+1) << 4);
        const uint8_t c[8] = { 0xFF, 0xFF, 0x00, 0x00, 0, 0, 0, 0 }; std::memcpy(blk + 8, c, 8);
        DecodeBlock(Kind::BC2, blk, out); bool ok = true;
        for (int i = 0; i < 16; ++i) ok &= out[i*4+3] == i * 17 && out[i*4] == 255;
        Report("BC2 explicit alpha", ok);
    }
    { // BC3: a0=0 < a1=255 would be 6-mode; use 8-mode a0=255,a1=0 with indices 0..7 then 7..0
        blk[0] = 255; blk[1] = 0; uint64_t idx = 0; const int seq[16] = { 0,2,3,4,5,6,7,1, 1,7,6,5,4,3,2,0 };
        for (int i = 0; i < 16; ++i) idx |= uint64_t(seq[i]) << (3 * i);
        for (int i = 0; i < 6; ++i) blk[2 + i] = uint8_t(idx >> (8 * i));
        const uint8_t c[8] = { 0xFF, 0xFF, 0x00, 0x00, 0, 0, 0, 0 }; std::memcpy(blk + 8, c, 8);
        DecodeBlock(Kind::BC3, blk, out);
        const int pal[8] = { 255, 0, 219, 182, 146, 109, 73, 36 }; // ((7-k)*255)/7 rounded
        bool ok = true; for (int i = 0; i < 16; ++i) ok &= out[i*4+3] == pal[seq[i]] && out[i*4] == 255;
        Report("BC3 alpha ramp", ok);
    }
}

static void PackBC4(uint8_t* b, int a0, int a1, const int* idx) {
    b[0] = uint8_t(a0); b[1] = uint8_t(a1); uint64_t v = 0;
    for (int i = 0; i < 16; ++i) v |= uint64_t(idx[i] & 7) << (3 * i);
    for (int i = 0; i < 6; ++i) b[2 + i] = uint8_t(v >> (8 * i));
}

static void TestBC4BC5() {
    int idx[16]; for (int i = 0; i < 16; ++i) idx[i] = i & 7;
    uint8_t blk[16], out[32];
    { PackBC4(blk, 70, 0, idx); DecodeBlock(Kind::BC4U, blk, out); // 8-mode: 70,0,60,50,40,30,20,10
      const int e[8] = { 70, 0, 60, 50, 40, 30, 20, 10 }; bool ok = true; for (int i = 0; i < 16; ++i) ok &= out[i] == e[i & 7];
      Report("BC4U 8-alpha mode", ok); }
    { PackBC4(blk, 0, 50, idx); DecodeBlock(Kind::BC4U, blk, out); // 6-mode: 0,50,10,20,30,40,0,255
      const int e[8] = { 0, 50, 10, 20, 30, 40, 0, 255 }; bool ok = true; for (int i = 0; i < 16; ++i) ok &= out[i] == e[i & 7];
      Report("BC4U 6-alpha mode", ok); }
    { PackBC4(blk, 70, -70, idx); DecodeBlock(Kind::BC4S, blk, out); // 8-mode: 70,-70,50,30,10,-10,-30,-50
      const int e[8] = { 70, -70, 50, 30, 10, -10, -30, -50 }; bool ok = true; for (int i = 0; i < 16; ++i) ok &= int8_t(out[i]) == e[i & 7];
      Report("BC4S 8-alpha mode", ok); }
    { PackBC4(blk, -128, 22, idx); DecodeBlock(Kind::BC4S, blk, out); // a0=-128 -> -127; 6-mode: -127,22,-97,-67,-38,-8,-127,127
      const int e[8] = { -127, 22, -97, -67, -38, -8, -127, 127 }; bool ok = true; for (int i = 0; i < 16; ++i) ok &= int8_t(out[i]) == e[i & 7];
      Report("BC4S 6-alpha mode (-128 clamp)", ok); }
    { PackBC4(blk, -127, -128, idx); DecodeBlock(Kind::BC4S, blk, out); // raw a0 > a1 -> 8-mode, all -127 (not 6-mode +127)
      bool ok = true; for (int i = 0; i < 16; ++i) ok &= int8_t(out[i]) == -127;
      Report("BC4S mode select on raw -127/-128", ok); }
    { PackBC4(blk, 70, 0, idx); PackBC4(blk + 8, 0, 50, idx); DecodeBlock(Kind::BC5U, blk, out);
      const int r[8] = { 70, 0, 60, 50, 40, 30, 20, 10 }, g[8] = { 0, 50, 10, 20, 30, 40, 0, 255 }; bool ok = true;
      for (int i = 0; i < 16; ++i) ok &= out[i*2] == r[i & 7] && out[i*2+1] == g[i & 7];
      Report("BC5U two channels", ok); }
    { PackBC4(blk, 70, -70, idx); PackBC4(blk + 8, -70, 70, idx); DecodeBlock(Kind::BC5S, blk, out); // G: 6-mode -70,70,-42,-14,14,42,-127,127
      const int r[8] = { 70, -70, 50, 30, 10, -10, -30, -50 }, g[8] = { -70, 70, -42, -14, 14, 42, -127, 127 }; bool ok = true;
      for (int i = 0; i < 16; ++i) ok &= int8_t(out[i*2]) == r[i & 7] && int8_t(out[i*2+1]) == g[i & 7];
      Report("BC5S two channels", ok); }
}

static void TestBC7() {
    uint8_t out[64];
    { // Mode 6: RGBA 7-bit endpoints + unique P bit -> 8-bit, 4-bit indices (anchor 3 bits)
        const unsigned e7[2][4] = { { 10, 100, 127, 0 }, { 120, 5, 64, 127 } }; const unsigned p[2] = { 1, 0 };
        BitWriter w; w.Put(1 << 6, 7);
        for (int ch = 0; ch < 4; ++ch) for (int e = 0; e < 2; ++e) w.Put(e7[e][ch], 7);
        w.Put(p[0], 1); w.Put(p[1], 1);
        unsigned idx[16]; for (int i = 0; i < 16; ++i) idx[i] = i == 0 ? 5 : (i * 7) & 15;
        for (int i = 0; i < 16; ++i) w.Put(idx[i], i ? 4 : 3);
        bool ok = w.pos == 128; DecodeBlock(Kind::BC7, w.b, out);
        for (int i = 0; i < 16; ++i) for (int ch = 0; ch < 4; ++ch)
            ok &= out[i*4+ch] == Lerp(e7[0][ch] << 1 | p[0], e7[1][ch] << 1 | p[1], W4[idx[i]]);
        Report("BC7 mode 6 bit-packed", ok);
    }
    { // Mode 1: 2 subsets, shape 17 (anchor of subset 1 at texel 2), RGB 6-bit + shared P per subset, 3-bit indices
        const unsigned shape = 17;
        const int part17[16] = { 0,1,1,1, 0,0,0,1, 0,0,0,0, 0,0,0,0 }; // g_aPartitionTable[1][17]
        const unsigned e6[4][3] = { { 0, 0, 0 }, { 63, 10, 20 }, { 5, 60, 33 }, { 40, 1, 63 } }; // A0,B0,A1,B1
        const unsigned p[2] = { 1, 0 };
        BitWriter w; w.Put(2, 2); w.Put(shape, 6);
        for (int ch = 0; ch < 3; ++ch) for (int e = 0; e < 4; ++e) w.Put(e6[e][ch], 6);
        w.Put(p[0], 1); w.Put(p[1], 1);
        unsigned idx[16]; for (int i = 0; i < 16; ++i) idx[i] = (i == 0 || i == 2) ? 3 : (i * 3) & 7;
        for (int i = 0; i < 16; ++i) w.Put(idx[i], (i == 0 || i == 2) ? 2 : 3);
        bool ok = w.pos == 128; DecodeBlock(Kind::BC7, w.b, out);
        auto unq = [](unsigned c7) { c7 <<= 1; return (c7 | c7 >> 7) & 255; };
        for (int i = 0; i < 16; ++i) {
            const int r = part17[i];
            for (int ch = 0; ch < 3; ++ch)
                ok &= out[i*4+ch] == Lerp(unq(e6[r*2][ch] << 1 | p[r]), unq(e6[r*2+1][ch] << 1 | p[r]), W3[idx[i]]);
            ok &= out[i*4+3] == 255;
        }
        Report("BC7 mode 1 two-subset partition", ok);
    }
    { // Mode 4: rotation 1 (r<->a), index-selection 1 (color uses 3-bit set, alpha uses 2-bit set)
        const unsigned e5[2][3] = { { 3, 31, 16 }, { 28, 0, 9 } }, a6[2] = { 63, 2 };
        BitWriter w; w.Put(1 << 4, 5); w.Put(1, 2); w.Put(1, 1);
        for (int ch = 0; ch < 3; ++ch) for (int e = 0; e < 2; ++e) w.Put(e5[e][ch], 5);
        w.Put(a6[0], 6); w.Put(a6[1], 6);
        unsigned i2[16], i3[16];
        for (int i = 0; i < 16; ++i) { i2[i] = i ? (i * 3) & 3 : 1; i3[i] = i ? (i * 5) & 7 : 2; }
        for (int i = 0; i < 16; ++i) w.Put(i2[i], i ? 2 : 1);
        for (int i = 0; i < 16; ++i) w.Put(i3[i], i ? 3 : 2);
        bool ok = w.pos == 128; DecodeBlock(Kind::BC7, w.b, out);
        static const int W2[] = { 0, 21, 43, 64 };
        for (int i = 0; i < 16; ++i) {
            unsigned px[4];
            for (int ch = 0; ch < 3; ++ch) px[ch] = Lerp(e5[0][ch] << 3 | e5[0][ch] >> 2, e5[1][ch] << 3 | e5[1][ch] >> 2, W3[i3[i]]);
            px[3] = Lerp(a6[0] << 2 | a6[0] >> 4, a6[1] << 2 | a6[1] >> 4, W2[i2[i]]);
            ok &= out[i*4] == px[3] && out[i*4+1] == px[1] && out[i*4+2] == px[2] && out[i*4+3] == px[0];
        }
        Report("BC7 mode 4 rotation + index selector", ok);
    }
    { // Mode 8 (all-zero first byte) -> transparent black
        uint8_t blk[16] = {}; std::memset(out, 0xAB, 64); DecodeBlock(Kind::BC7, blk, out);
        bool ok = true; for (int i = 0; i < 64; ++i) ok &= out[i] == 0;
        Report("BC7 reserved mode 8", ok);
    }
}

// Reference formulas from DirectXTex D3DX_BC6H::Unquantize / FinishUnquantize (+INT2F16).
static int Unq6(int comp, int bits, bool sgn) {
    if (sgn) { int s = comp < 0; if (s) comp = -comp; int u = comp == 0 ? 0 : comp >= (1 << (bits - 1)) - 1 ? 0x7FFF : ((comp << 15) + 0x4000) >> (bits - 1); return s ? -u : u; }
    return comp == 0 ? 0 : comp == (1 << bits) - 1 ? 0xFFFF : ((comp << 16) + 0x8000) >> bits;
}
static unsigned Fin6(int c, bool sgn) { return sgn ? (c < 0 ? 0x8000u | unsigned(((-c) * 31) >> 5) : unsigned((c * 31) >> 5)) : unsigned((c * 31) >> 6); }

static void TestBC6H() {
    uint8_t out[128];
    // Mode 11 (0x03): 5 mode bits, RW GW BW (10 each), RX GX BX (10 each, raw, not transformed), then 4-bit indices (anchor 3 bits)
    const unsigned A[3] = { 0, 300, 777 }, B[3] = { 1023, 5, 512 };
    BitWriter w; w.Put(0x03, 5);
    for (int c = 0; c < 3; ++c) w.Put(A[c], 10);
    for (int c = 0; c < 3; ++c) w.Put(B[c], 10);
    unsigned idx[16]; for (int i = 0; i < 16; ++i) idx[i] = i == 0 ? 0 : i == 15 ? 15 : (i * 5) & 15;
    for (int i = 0; i < 16; ++i) w.Put(idx[i], i ? 4 : 3);
    {
        bool ok = w.pos == 128; DecodeBlock(Kind::BC6HU, w.b, out);
        for (int i = 0; i < 16; ++i) {
            for (int c = 0; c < 3; ++c) {
                const int a = Unq6(int(A[c]), 10, false), b = Unq6(int(B[c]), 10, false), wt = W4[idx[i]];
                ok &= Rd16(out + i*8 + c*2) == Fin6((a * (64 - wt) + b * wt + 32) >> 6, false);
            }
            ok &= Rd16(out + i*8 + 6) == 0x3C00;
        }
        // literal anchors: texel 0 red = 0 (idx 0, A.r=0); texel 15 red = 0x7BFF (B.r=1023 -> 0xFFFF*31>>6 = 65504.0h)
        ok &= Rd16(out) == 0 && Rd16(out + 15*8) == 0x7BFF;
        Report("BC6HU mode 11 bit-packed", ok);
    }
    { // Same bits as signed: A.r=0, B.r=1023 -> -1 -> unq -96 -> fin -93 -> 0x805D at idx 15
        bool ok = true; DecodeBlock(Kind::BC6HS, w.b, out);
        auto sx = [](int v) { return v & 512 ? v - 1024 : v; };
        for (int i = 0; i < 16; ++i) for (int c = 0; c < 3; ++c) {
            const int a = Unq6(sx(int(A[c])), 10, true), b = Unq6(sx(int(B[c])), 10, true), wt = W4[idx[i]];
            ok &= Rd16(out + i*8 + c*2) == Fin6((a * (64 - wt) + b * wt + 32) >> 6, true);
        }
        ok &= Rd16(out + 15*8) == 0x805D;
        Report("BC6HS mode 11 bit-packed", ok);
    }
    { // Mode 14 (0x0F): transformed, A 16-bit (bits 15..10 stored reversed), B = A + 4-bit signed delta (wraps)
        const unsigned A6[3] = { 0xFC01, 5, 0x1234 }; const int dl[3] = { -3, -7, 7 };
        BitWriter m; m.Put(0x0F, 5);
        for (int c = 0; c < 3; ++c) m.Put(A6[c] & 1023, 10);
        for (int c = 0; c < 3; ++c) { m.Put(unsigned(dl[c]) & 15, 4); for (int k = 15; k >= 10; --k) m.Put((A6[c] >> k) & 1, 1); }
        for (int i = 0; i < 16; ++i) m.Put(idx[i], i ? 4 : 3);
        bool ok = m.pos == 128; DecodeBlock(Kind::BC6HU, m.b, out);
        for (int i = 0; i < 16; ++i) for (int c = 0; c < 3; ++c) {
            const int a = int(A6[c]), b = int((A6[c] + unsigned(dl[c])) & 0xFFFF), wt = W4[idx[i]]; // 16-bit: Unquantize is identity
            ok &= Rd16(out + i*8 + c*2) == Fin6((a * (64 - wt) + b * wt + 32) >> 6, false);
        }
        ok &= Rd16(out + 15*8 + 2) == ((0xFFFEu * 31) >> 6); // green wrapped 5-7 -> 0xFFFE
        Report("BC6HU mode 14 transformed", ok);
    }
    { // Reserved mode 0x13 -> (0,0,0,1.0h)
        BitWriter r; r.Put(0x13, 5); DecodeBlock(Kind::BC6HU, r.b, out); bool ok = true;
        for (int i = 0; i < 16; ++i) ok &= Rd16(out+i*8) == 0 && Rd16(out+i*8+2) == 0 && Rd16(out+i*8+4) == 0 && Rd16(out+i*8+6) == 0x3C00;
        Report("BC6H reserved mode", ok);
    }
}

static void TestRegion() {
    // 2x2 mip in one BC4 block: texel (x,y) = idx y*4+x
    int idx[16]; for (int i = 0; i < 16; ++i) idx[i] = i & 7;
    uint8_t blk[8]; PackBC4(blk, 70, 0, idx); const int pal[8] = { 70, 0, 60, 50, 40, 30, 20, 10 };
    {
        uint8_t dst[3 * 2]; std::memset(dst, 0xEE, sizeof dst); // pitch 3 to detect overwrite
        DecodeRegion(Kind::BC4U, blk, 8, 1, 1, dst, 3, 2, 2);
        bool ok = dst[0] == pal[0] && dst[1] == pal[1] && dst[2] == 0xEE && dst[3] == pal[4] && dst[4] == pal[5] && dst[5] == 0xEE;
        Report("DecodeRegion 2x2 partial mip", ok);
    }
    {
        uint8_t blk1[8] = { 0xFF, 0xFF, 0x00, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 }; uint8_t dst[8]; std::memset(dst, 0xEE, 8);
        DecodeRegion(Kind::BC1, blk1, 8, 1, 1, dst, 4, 1, 1);
        bool ok = dst[0] == 255 && dst[1] == 255 && dst[2] == 255 && dst[3] == 255 && dst[4] == 0xEE;
        Report("DecodeRegion 1x1 partial mip", ok);
    }
    { // 6x5 image from 2x2 BC4 blocks with distinct a0 per block; check clip and placement
        uint8_t src[4 * 8]; int zero[16] = {};
        for (int b = 0; b < 4; ++b) PackBC4(src + b * 8, 10 + b, 0, zero); // all texels = a0
        uint8_t dst[8 * 6]; std::memset(dst, 0xEE, sizeof dst);
        DecodeRegion(Kind::BC4U, src, 16, 2, 2, dst, 8, 6, 5);
        bool ok = true;
        for (int y = 0; y < 6; ++y) for (int x = 0; x < 8; ++x) {
            const int v = dst[y * 8 + x];
            ok &= (x < 6 && y < 5) ? v == 10 + (y >= 4) * 2 + (x >= 4) : v == 0xEE;
        }
        Report("DecodeRegion 6x5 multi-block clip", ok);
    }
}

int main() {
    Report("BytesPerTexel/BlockBytes", DecodedBytesPerTexel(Kind::BC1) == 4 && DecodedBytesPerTexel(Kind::BC5S) == 2 && DecodedBytesPerTexel(Kind::BC6HS) == 8 &&
           DecodedBytesPerTexel(Kind::BC4U) == 1 && BlockBytes(Kind::BC4S) == 8 && BlockBytes(Kind::BC5U) == 16 && BlockBytes(Kind::BC7) == 16);
    TestBC1(); TestBC2BC3(); TestBC4BC5(); TestBC7(); TestBC6H(); TestRegion();
    std::printf("%s (%d failed)\n", g_fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
