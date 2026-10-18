#include "ragekit/image.h"
#include "ragekit/error.h"
#include "miniz.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace rk {

// ================================================================ block decoders

namespace {

// 5 and 6 bit channels widened by repeating their top bits (= round(v * 255 / 31), what D3D does); the BC1 and BC4
// palettes are the endpoints' exact mixes rounded to the nearest 8-bit value (D3D interpolates in float and rounds).
inline uint8_t expand5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
inline uint8_t expand6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
inline void rgb565(uint16_t c, uint8_t out[3]) {
    out[0] = expand5((c >> 11) & 31);
    out[1] = expand6((c >> 5) & 63);
    out[2] = expand5(c & 31);
}

// Writes a 4x4 block of RGBA into img at (bx*4, by*4), clipping at the edges.
struct BlockSink {
    Image& img;
    uint32_t bx, by;
    void put(int x, int y, const uint8_t rgba[4]) {
        uint32_t px = bx * 4 + x, py = by * 4 + y;
        if (px < img.width && py < img.height) std::memcpy(img.px(px, py), rgba, 4);
    }
};

void decode_bc1_block(const uint8_t* s, uint8_t colors[16][4], bool allow_alpha) {
    uint16_t c0 = (uint16_t)(s[0] | (s[1] << 8)), c1 = (uint16_t)(s[2] | (s[3] << 8));
    uint8_t p[4][4];
    rgb565(c0, p[0]); p[0][3] = 255;
    rgb565(c1, p[1]); p[1][3] = 255;
    if (c0 > c1 || !allow_alpha) {
        for (int i = 0; i < 3; i++) {
            p[2][i] = (uint8_t)((2 * p[0][i] + p[1][i] + 1) / 3);
            p[3][i] = (uint8_t)((p[0][i] + 2 * p[1][i] + 1) / 3);
        }
        p[2][3] = p[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) { p[2][i] = (uint8_t)((p[0][i] + p[1][i] + 1) / 2); p[3][i] = 0; }
        p[2][3] = 255; p[3][3] = 0;
    }
    uint32_t bits = (uint32_t)(s[4] | (s[5] << 8) | (s[6] << 16) | ((uint32_t)s[7] << 24));
    for (int i = 0; i < 16; i++) {
        int idx = (bits >> (2 * i)) & 3;
        std::memcpy(colors[i], p[idx], 4);
    }
}

void decode_bc4_block(const uint8_t* s, uint8_t out[16]) {
    uint8_t a0 = s[0], a1 = s[1];
    uint8_t p[8];
    p[0] = a0; p[1] = a1;
    if (a0 > a1) for (int i = 1; i < 7; i++) p[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1 + 3) / 7);
    else {
        for (int i = 1; i < 5; i++) p[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1 + 2) / 5);
        p[6] = 0; p[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)s[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = p[(bits >> (3 * i)) & 7];
}

// ---- BC7

struct Bits {
    const uint8_t* d;
    int pos = 0;
    uint32_t get(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++, pos++) v |= (uint32_t)((d[pos >> 3] >> (pos & 7)) & 1) << i;
        return v;
    }
};

const uint8_t kBC7Partitions2[64][16] = {
    {0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1},{0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1},{0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1},{0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1},{0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1},
    {0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1},{0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
    {0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1},{0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0},{0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0},
    {0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0},{0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0},{0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0},{0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1},
    {0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0},{0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0},{0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0},{0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0},
    {0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0},{0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0},{0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0},{0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0},
    {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},{0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1},{0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0},{0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0},
    {0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0},{0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0},{0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1},{0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1},
    {0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0},{0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0},{0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0},{0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0},
    {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0},{0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1},{0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1},{0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
    {0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0},{0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0},{0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0},{0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0},
    {0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1},{0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0},{0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0},
    {0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1},{0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1},{0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1},{0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1},
    {0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1},{0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0},{0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0},{0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1}};

const uint8_t kBC7Partitions3[64][16] = {
    {0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2},{0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1},{0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1},{0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2},{0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},{0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1},{0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
    {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2},{0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
    {0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2},{0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2},{0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2},{0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
    {0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2},{0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},{0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2},{0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
    {0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2},{0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1},{0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2},{0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
    {0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0},{0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2},{0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0},{0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
    {0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2},{0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},{0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1},{0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
    {0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2},{0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1},{0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2},{0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
    {0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0},{0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},{0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0},{0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
    {0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1},{0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1},{0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
    {0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1},{0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1},{0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1},{0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
    {0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2},{0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1},{0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2},{0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
    {0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2},{0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},{0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
    {0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2},{0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2},{0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2},{0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
    {0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1},{0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2},{0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2},{0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0}};

const uint8_t kBC7Anchor2[64] = {15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,2,8,2,2,8,8,15,2,8,2,2,8,8,2,2,15,15,6,8,2,8,15,15,2,8,2,2,2,15,15,6,6,2,6,8,15,15,2,2,15,15,15,15,15,2,2,15};
const uint8_t kBC7Anchor3a[64] = {3,3,15,15,8,3,15,15,8,8,6,6,6,5,3,3,3,3,8,15,3,3,6,10,5,8,8,6,8,5,15,15,8,15,3,5,6,10,8,15,15,3,15,5,15,15,15,15,3,15,5,5,5,8,5,10,5,10,8,13,15,12,3,3};
const uint8_t kBC7Anchor3b[64] = {15,8,8,3,15,15,3,8,15,15,15,15,15,15,15,8,15,8,15,3,15,8,15,8,3,15,6,10,15,15,10,8,15,3,15,10,10,8,9,10,6,15,8,15,3,6,6,8,15,3,15,15,15,15,15,15,15,15,15,15,3,15,15,8};
const uint16_t kW2[4] = {0, 21, 43, 64};
const uint16_t kW3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
const uint16_t kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

uint8_t expand(uint32_t v, int bits) {
    if (bits >= 8) return (uint8_t)v;
    v <<= (8 - bits);
    return (uint8_t)(v | (v >> bits));
}

void decode_bc7_block(const uint8_t* s, uint8_t out[16][4]) {
    Bits b{s};
    int mode = 0;
    while (mode < 8 && b.get(1) == 0) mode++;
    if (mode == 8) { std::memset(out, 0, 64); return; }
    static const struct { int subsets, pbits, rot, isel, cbits, abits, pmode, ibits, ibits2; } M[8] = {
        {3, 4, 0, 0, 4, 0, 1, 3, 0}, {2, 6, 0, 0, 6, 0, 2, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 2, 0}, {2, 6, 0, 0, 7, 0, 1, 2, 0},
        {1, 0, 2, 1, 5, 6, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 2, 2}, {1, 0, 0, 0, 7, 7, 1, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 2, 0}};
    auto& m = M[mode];
    int partition = b.get(m.pbits);
    int rotation = b.get(m.rot);
    int isel = b.get(m.isel);
    int ns = m.subsets;
    uint32_t ep[3][2][4]; // subset, endpoint, channel
    for (int c = 0; c < 3; c++)
        for (int si = 0; si < ns; si++)
            for (int e = 0; e < 2; e++) ep[si][e][c] = b.get(m.cbits);
    for (int si = 0; si < ns; si++)
        for (int e = 0; e < 2; e++) ep[si][e][3] = m.abits ? b.get(m.abits) : 255;
    int cbits = m.cbits, abits = m.abits;
    if (m.pmode == 1) { // per-endpoint p-bit
        for (int si = 0; si < ns; si++)
            for (int e = 0; e < 2; e++) {
                uint32_t p = b.get(1);
                for (int c = 0; c < 4; c++) if (c < 3 || abits) ep[si][e][c] = (ep[si][e][c] << 1) | p;
            }
        cbits++; if (abits) abits++;
    } else if (m.pmode == 2) { // shared p-bit
        for (int si = 0; si < ns; si++) {
            uint32_t p = b.get(1);
            for (int e = 0; e < 2; e++)
                for (int c = 0; c < 4; c++) if (c < 3 || abits) ep[si][e][c] = (ep[si][e][c] << 1) | p;
        }
        cbits++; if (abits) abits++;
    }
    uint8_t epx[3][2][4];
    for (int si = 0; si < ns; si++)
        for (int e = 0; e < 2; e++) {
            for (int c = 0; c < 3; c++) epx[si][e][c] = expand(ep[si][e][c], cbits);
            epx[si][e][3] = abits ? expand(ep[si][e][3], abits) : 255;
        }
    const uint8_t* part = ns == 1 ? nullptr : (ns == 2 ? kBC7Partitions2[partition] : kBC7Partitions3[partition]);
    auto anchor = [&](int si) -> int {
        if (si == 0) return 0;
        if (ns == 2) return kBC7Anchor2[partition];
        return si == 1 ? kBC7Anchor3a[partition] : kBC7Anchor3b[partition];
    };
    int idx[16], idx2[16];
    for (int i = 0; i < 16; i++) {
        int si = part ? part[i] : 0;
        int bits = m.ibits - (anchor(si) == i ? 1 : 0);
        idx[i] = b.get(bits);
    }
    if (m.ibits2)
        for (int i = 0; i < 16; i++) idx2[i] = b.get(m.ibits2 - (i == 0 ? 1 : 0));
    auto weights = [](int bits) { return bits == 2 ? kW2 : bits == 3 ? kW3 : kW4; };
    for (int i = 0; i < 16; i++) {
        int si = part ? part[i] : 0;
        int ci = idx[i], ai = idx[i];
        int cb = m.ibits, ab = m.ibits;
        if (m.ibits2) {
            if (isel) { ci = idx2[i]; cb = m.ibits2; } else { ai = idx2[i]; ab = m.ibits2; }
        }
        uint16_t wc = weights(cb)[ci], wa = weights(ab)[ai];
        for (int c = 0; c < 3; c++)
            out[i][c] = (uint8_t)(((64 - wc) * epx[si][0][c] + wc * epx[si][1][c] + 32) >> 6);
        out[i][3] = (uint8_t)(((64 - wa) * epx[si][0][3] + wa * epx[si][1][3] + 32) >> 6);
        if (rotation) std::swap(out[i][3], out[i][rotation - 1]);
    }
}

} // namespace

// ================================================================ decoding

Image decode_texels(TextureFormat f, uint32_t w, uint32_t h, std::span<const uint8_t> data) {
    Image img;
    img.width = std::max(1u, w);
    img.height = std::max(1u, h);
    img.rgba.assign((size_t)img.width * img.height * 4, 0);
    size_t need = texture_level_size(f, w, h);
    if (data.size() < need) throw Error("texture data shorter than its level size");
    const uint8_t* s = data.data();
    if (texture_format_is_block(f)) {
        uint32_t bw = (img.width + 3) / 4, bh = (img.height + 3) / 4;
        uint32_t unit = texture_format_unit_bytes(f);
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t* blk = s + ((size_t)by * bw + bx) * unit;
                uint8_t px[16][4];
                switch (f) {
                    case TextureFormat::DXT1: decode_bc1_block(blk, px, true); break;
                    case TextureFormat::DXT3:
                        decode_bc1_block(blk + 8, px, false);
                        for (int i = 0; i < 16; i++) {
                            uint8_t a = (uint8_t)((blk[i / 2] >> ((i & 1) * 4)) & 15);
                            px[i][3] = (uint8_t)(a * 17);
                        }
                        break;
                    case TextureFormat::DXT5: {
                        decode_bc1_block(blk + 8, px, false);
                        uint8_t a[16];
                        decode_bc4_block(blk, a);
                        for (int i = 0; i < 16; i++) px[i][3] = a[i];
                        break;
                    }
                    case TextureFormat::ATI1: {
                        uint8_t r[16];
                        decode_bc4_block(blk, r);
                        for (int i = 0; i < 16; i++) { px[i][0] = px[i][1] = px[i][2] = r[i]; px[i][3] = 255; }
                        break;
                    }
                    case TextureFormat::ATI2: {
                        uint8_t r[16], g[16];
                        decode_bc4_block(blk, r);
                        decode_bc4_block(blk + 8, g);
                        for (int i = 0; i < 16; i++) {
                            float x = r[i] / 127.5f - 1, y = g[i] / 127.5f - 1;
                            float z = std::sqrt(std::max(0.f, 1 - x * x - y * y));
                            px[i][0] = r[i]; px[i][1] = g[i]; px[i][2] = (uint8_t)std::lround((z * 0.5f + 0.5f) * 255); px[i][3] = 255;
                        }
                        break;
                    }
                    case TextureFormat::BC7: decode_bc7_block(blk, px); break;
                    default: break;
                }
                BlockSink sink{img, bx, by};
                for (int i = 0; i < 16; i++) sink.put(i & 3, i >> 2, px[i]);
            }
        return img;
    }
    for (uint32_t y = 0; y < img.height; y++)
        for (uint32_t x = 0; x < img.width; x++) {
            uint8_t* d = img.px(x, y);
            size_t i = (size_t)y * img.width + x;
            switch (f) {
                case TextureFormat::A8R8G8B8: d[0] = s[i * 4 + 2]; d[1] = s[i * 4 + 1]; d[2] = s[i * 4 + 0]; d[3] = s[i * 4 + 3]; break;
                case TextureFormat::X8R8G8B8: d[0] = s[i * 4 + 2]; d[1] = s[i * 4 + 1]; d[2] = s[i * 4 + 0]; d[3] = 255; break;
                case TextureFormat::A8B8G8R8: std::memcpy(d, s + i * 4, 4); break;
                case TextureFormat::A1R5G5B5: {
                    uint16_t v = (uint16_t)(s[i * 2] | (s[i * 2 + 1] << 8));
                    d[0] = expand5((v >> 10) & 31); d[1] = expand5((v >> 5) & 31);
                    d[2] = expand5(v & 31); d[3] = (v & 0x8000) ? 255 : 0;
                    break;
                }
                case TextureFormat::A8: d[0] = d[1] = d[2] = 0; d[3] = s[i]; break;
                case TextureFormat::L8: d[0] = d[1] = d[2] = s[i]; d[3] = 255; break;
                default: throw Error(std::string("cannot decode texture format ") + texture_format_name(f));
            }
        }
    return img;
}

Image decode_texture(const Texture& t, uint32_t level) {
    uint32_t w, h;
    auto data = t.level(level, w, h);
    return decode_texels(t.format, w, h, data);
}

// ================================================================ encoding

namespace {

uint16_t to565(const float c[3]) {
    int r = (int)std::lround(std::clamp(c[0], 0.f, 255.f) * 31 / 255);
    int g = (int)std::lround(std::clamp(c[1], 0.f, 255.f) * 63 / 255);
    int b = (int)std::lround(std::clamp(c[2], 0.f, 255.f) * 31 / 255);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// A BC1 candidate: two 565 endpoints and the index of every pixel, with its squared error.
struct Bc1Pick {
    uint16_t c0 = 0, c1 = 0;
    uint8_t idx[16] = {};
    int err = INT32_MAX;
};

// The 4-colour palette exactly as the decoder builds it (index 2 = 2/3 c0, index 3 = 2/3 c1).
void bc1_palette(uint16_t c0, uint16_t c1, int p[4][3]) {
    uint8_t a[3], b[3];
    rgb565(c0, a); rgb565(c1, b);
    for (int c = 0; c < 3; c++) { p[0][c] = a[c]; p[1][c] = b[c]; p[2][c] = (2 * a[c] + b[c] + 1) / 3; p[3][c] = (a[c] + 2 * b[c] + 1) / 3; }
}

// Nearest palette entry per pixel for the endpoints c0, c1 (either order); keeps the result in best when it is better.
void bc1_try(const uint8_t px[16][4], uint16_t c0, uint16_t c1, Bc1Pick& best) {
    Bc1Pick t;
    t.c0 = c0; t.c1 = c1; t.err = 0;
    int p[4][3];
    bc1_palette(c0, c1, p);
    for (int i = 0; i < 16; i++) {
        int bd = INT32_MAX, bk = 0;
        for (int k = 0; k < 4; k++) {
            int d = 0;
            for (int c = 0; c < 3; c++) { int e = (int)px[i][c] - p[k][c]; d += e * e; }
            if (d < bd) { bd = d; bk = k; }
        }
        t.idx[i] = (uint8_t)bk;
        t.err += bd;
        if (t.err >= best.err) return;   // no better than what is kept
    }
    best = t;
}

// Moves the kept endpoints one 565 step at a time (either endpoint, any channel, either way) while a step lowers the
// error, at most four rounds: least squares works in floats and rounds once.
void bc1_refine(const uint8_t px[16][4], Bc1Pick& best) {
    static const uint16_t kStep[3] = {1 << 11, 1 << 5, 1};
    static const uint16_t kMask[3] = {31 << 11, 63 << 5, 31};
    for (int round = 0; round < 4; round++) {
        const Bc1Pick base = best;
        for (int e = 0; e < 2; e++)
            for (int c = 0; c < 3; c++)
                for (int s = -1; s <= 1; s += 2) {
                    const uint16_t v = e ? base.c1 : base.c0;
                    const int field = (v & kMask[c]) / kStep[c] + s, top = kMask[c] / kStep[c];
                    if (field < 0 || field > top) continue;
                    const uint16_t w = (uint16_t)((v & ~kMask[c]) | field * kStep[c]);
                    bc1_try(px, e ? base.c0 : w, e ? w : base.c1, best);
                }
        if (best.err >= base.err) break;
    }
}

// Least-squares endpoints for fixed indices (index k puts weight w[k] on c0): false when every pixel uses one weight.
bool bc1_fit(const uint8_t px[16][4], const uint8_t idx[16], float e0[3], float e1[3]) {
    static const float w[4] = {1.f, 0.f, 2.f / 3.f, 1.f / 3.f};
    float A = 0, B = 0, C = 0, X[3] = {}, Y[3] = {};
    for (int i = 0; i < 16; i++) {
        const float a = w[idx[i]], b = 1.f - a;
        A += a * a; B += a * b; C += b * b;
        for (int c = 0; c < 3; c++) { X[c] += a * px[i][c]; Y[c] += b * px[i][c]; }
    }
    const float det = A * C - B * B;
    if (std::abs(det) < 1e-4f) return false;
    for (int c = 0; c < 3; c++) { e0[c] = (C * X[c] - B * Y[c]) / det; e1[c] = (A * Y[c] - B * X[c]) / det; }
    return true;
}

// For one channel value v: the endpoint pair (a, b) of `bits` bits whose 2/3 mix decodes closest to v.
void bc1_solid_pair(int v, int bits, int& a, int& b) {
    const int top = (1 << bits) - 1;
    int bd = INT32_MAX;
    for (int x = 0; x <= top; x++)
        for (int y = 0; y <= top; y++) {
            const int ex = bits == 5 ? expand5((uint32_t)x) : expand6((uint32_t)x), ey = bits == 5 ? expand5((uint32_t)y) : expand6((uint32_t)y);
            const int d = std::abs((2 * ex + ey + 1) / 3 - v);
            if (d < bd) { bd = d; a = x; b = y; }
        }
}

// The best block for 16 pixels of one colour: a 2/3 mix of two endpoints gets closer than the nearest 565 colour.
void bc1_solid(const uint8_t px[16][4], Bc1Pick& best) {
    struct Tabs { int t5[256][2], t6[256][2]; };
    static const Tabs tabs = [] {   // a function-local static: built once, safe with several encoding threads
        Tabs t;
        for (int v = 0; v < 256; v++) { bc1_solid_pair(v, 5, t.t5[v][0], t.t5[v][1]); bc1_solid_pair(v, 6, t.t6[v][0], t.t6[v][1]); }
        return t;
    }();
    const int r = px[0][0], g = px[0][1], b = px[0][2];
    const uint16_t c0 = (uint16_t)((tabs.t5[r][0] << 11) | (tabs.t6[g][0] << 5) | tabs.t5[b][0]);
    const uint16_t c1 = (uint16_t)((tabs.t5[r][1] << 11) | (tabs.t6[g][1] << 5) | tabs.t5[b][1]);
    bc1_try(px, c0, c1, best);
}

// Encodes 16 RGBA pixels as a BC1 colour block (4 colours, no punch-through alpha). The endpoints start at the pixels
// furthest apart along the colours' principal axis (a bounding box picks the wrong diagonal for a red-green ramp) and at
// the box corners, two rounds of least squares refit them to the chosen indices, and 565 steps refine the best; every
// candidate is scored by the palette the decoder builds.
void encode_bc1_block(const uint8_t px[16][4], uint8_t out[8]) {
    Bc1Pick best;
    float mean[3] = {}, mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    bool solid = true;
    for (int i = 0; i < 16; i++) {
        for (int c = 0; c < 3; c++) {
            mean[c] += px[i][c] / 16.f;
            mn[c] = std::min(mn[c], (float)px[i][c]); mx[c] = std::max(mx[c], (float)px[i][c]);
        }
        if (px[i][0] != px[0][0] || px[i][1] != px[0][1] || px[i][2] != px[0][2]) solid = false;
    }
    if (solid) bc1_solid(px, best);
    else {
        float cov[6] = {};   // xx xy xz yy yz zz
        for (int i = 0; i < 16; i++) {
            const float d[3] = {px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2]};
            cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2];
            cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2];
        }
        // power iteration towards the largest eigenvector, from the covariance column of the channel that varies most (the
        // box diagonal can be orthogonal to the axis: red-green ramp, diagonal (1,1,0), axis (-1,1,0))
        static const int kCol[3][3] = {{0, 1, 2}, {1, 3, 4}, {2, 4, 5}};
        const int col = cov[0] >= cov[3] && cov[0] >= cov[5] ? 0 : cov[3] >= cov[5] ? 1 : 2;
        float ax[3] = {cov[kCol[col][0]], cov[kCol[col][1]], cov[kCol[col][2]]};
        for (int it = 0; it < 8; it++) {
            const float n[3] = {cov[0] * ax[0] + cov[1] * ax[1] + cov[2] * ax[2], cov[1] * ax[0] + cov[3] * ax[1] + cov[4] * ax[2],
                                cov[2] * ax[0] + cov[4] * ax[1] + cov[5] * ax[2]};
            const float l = std::max({std::abs(n[0]), std::abs(n[1]), std::abs(n[2])});
            if (l < 1e-6f) break;
            for (int c = 0; c < 3; c++) ax[c] = n[c] / l;
        }
        int lo = 0, hi = 0;
        float plo = 1e30f, phi = -1e30f;
        for (int i = 0; i < 16; i++) {
            const float t = px[i][0] * ax[0] + px[i][1] * ax[1] + px[i][2] * ax[2];
            if (t < plo) { plo = t; lo = i; }
            if (t > phi) { phi = t; hi = i; }
        }
        float e0[3], e1[3];
        for (int c = 0; c < 3; c++) { e0[c] = px[hi][c]; e1[c] = px[lo][c]; }
        bc1_try(px, to565(e0), to565(e1), best);
        bc1_try(px, to565(mx), to565(mn), best);
        for (int round = 0; round < 2; round++) {
            if (!bc1_fit(px, best.idx, e0, e1)) break;
            bc1_try(px, to565(e0), to565(e1), best);
        }
        bc1_refine(px, best);
    }
    uint16_t c0 = best.c0, c1 = best.c1;
    uint32_t bits = 0;
    if (c0 == c1) bits = 0;   // one colour: index 0 everywhere decodes the same in either mode
    else {
        // 4-colour mode needs c0 > c1: swapping the endpoints swaps indices 0 / 1 and 2 / 3
        const bool swap = c0 < c1;
        if (swap) std::swap(c0, c1);
        for (int i = 0; i < 16; i++) bits |= (uint32_t)(swap ? best.idx[i] ^ 1 : best.idx[i]) << (2 * i);
    }
    out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8); out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
    out[4] = (uint8_t)bits; out[5] = (uint8_t)(bits >> 8); out[6] = (uint8_t)(bits >> 16); out[7] = (uint8_t)(bits >> 24);
}

// Nearest of the 8 BC4 values for each input; returns the squared error.
int bc4_indices(const uint8_t v[16], const uint8_t p[8], uint64_t& bits) {
    int err = 0;
    bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = INT32_MAX;
        for (int k = 0; k < 8; k++) { int d = std::abs((int)v[i] - p[k]); if (d < bd) { bd = d; best = k; } }
        bits |= (uint64_t)best << (3 * i);
        err += bd * bd;
    }
    return err;
}

// Encodes 16 single-channel values as a BC4 block: 8 interpolated values between min and max, or (when the block holds
// 0 or 255 next to other values, as cutout edges do) 6 interpolated values plus exact 0 and 255, whichever is closer.
void encode_bc4_block(const uint8_t v[16], uint8_t out[8]) {
    uint8_t mn = 255, mx = 0, imn = 255, imx = 0;   // imn / imx: without the exact 0 and 255
    for (int i = 0; i < 16; i++) {
        mn = std::min(mn, v[i]); mx = std::max(mx, v[i]);
        if (v[i] != 0 && v[i] != 255) { imn = std::min(imn, v[i]); imx = std::max(imx, v[i]); }
    }
    if (mn == mx) { out[0] = out[1] = mn; std::memset(out + 2, 0, 6); return; }
    uint8_t p[8];
    p[0] = mx; p[1] = mn;   // a0 > a1: 8 interpolated values
    for (int i = 1; i < 7; i++) p[i + 1] = (uint8_t)(((7 - i) * mx + i * mn + 3) / 7);
    uint64_t bits;
    int err = bc4_indices(v, p, bits);
    uint8_t a0 = mx, a1 = mn;
    if (err > 0 && imn <= imx && (mn == 0 || mx == 255)) {
        uint8_t q[8];
        q[0] = imn; q[1] = imx;   // a0 <= a1: 6 interpolated values, then 0 and 255
        for (int i = 1; i < 5; i++) q[i + 1] = (uint8_t)(((5 - i) * imn + i * imx + 2) / 5);
        q[6] = 0; q[7] = 255;
        uint64_t qbits;
        if (bc4_indices(v, q, qbits) < err) { a0 = imn; a1 = imx; bits = qbits; }
    }
    out[0] = a0; out[1] = a1;
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(bits >> (8 * i));
}

void fetch_block(const Image& img, uint32_t bx, uint32_t by, uint8_t px[16][4]) {
    for (int i = 0; i < 16; i++) {
        uint32_t x = std::min(bx * 4 + (i & 3), img.width - 1), y = std::min(by * 4 + (i >> 2), img.height - 1);
        std::memcpy(px[i], img.px(x, y), 4);
    }
}

Bytes encode_level(const Image& img, TextureFormat f) {
    Bytes out(texture_level_size(f, img.width, img.height));
    if (texture_format_is_block(f)) {
        uint32_t bw = (img.width + 3) / 4, bh = (img.height + 3) / 4;
        uint32_t unit = texture_format_unit_bytes(f);
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                uint8_t px[16][4];
                fetch_block(img, bx, by, px);
                uint8_t* o = out.data() + ((size_t)by * bw + bx) * unit;
                uint8_t ch[16];
                switch (f) {
                    case TextureFormat::DXT1: encode_bc1_block(px, o); break;
                    case TextureFormat::DXT5:
                        for (int i = 0; i < 16; i++) ch[i] = px[i][3];
                        encode_bc4_block(ch, o);
                        encode_bc1_block(px, o + 8);
                        break;
                    case TextureFormat::DXT3:
                        for (int i = 0; i < 16; i += 2) o[i / 2] = (uint8_t)((px[i][3] + 8) / 17 | (px[i + 1][3] + 8) / 17 << 4);   // nearest of a * 17
                        encode_bc1_block(px, o + 8);
                        break;
                    case TextureFormat::ATI1:
                        for (int i = 0; i < 16; i++) ch[i] = px[i][0];
                        encode_bc4_block(ch, o);
                        break;
                    case TextureFormat::ATI2:
                        for (int i = 0; i < 16; i++) ch[i] = px[i][0];
                        encode_bc4_block(ch, o);
                        for (int i = 0; i < 16; i++) ch[i] = px[i][1];
                        encode_bc4_block(ch, o + 8);
                        break;
                    default: throw Error(std::string("cannot encode texture format ") + texture_format_name(f));
                }
            }
        return out;
    }
    for (uint32_t y = 0; y < img.height; y++)
        for (uint32_t x = 0; x < img.width; x++) {
            const uint8_t* s = img.px(x, y);
            size_t i = (size_t)y * img.width + x;
            switch (f) {
                case TextureFormat::A8R8G8B8: out[i * 4] = s[2]; out[i * 4 + 1] = s[1]; out[i * 4 + 2] = s[0]; out[i * 4 + 3] = s[3]; break;
                case TextureFormat::X8R8G8B8: out[i * 4] = s[2]; out[i * 4 + 1] = s[1]; out[i * 4 + 2] = s[0]; out[i * 4 + 3] = 255; break;
                case TextureFormat::A8B8G8R8: std::memcpy(&out[i * 4], s, 4); break;
                case TextureFormat::A8: out[i] = s[3]; break;
                case TextureFormat::L8: out[i] = (uint8_t)((s[0] * 299 + s[1] * 587 + s[2] * 114) / 1000); break;
                default: throw Error(std::string("cannot encode texture format ") + texture_format_name(f));
            }
        }
    return out;
}

} // namespace

Image downsample(const Image& img) {
    Image o;
    o.width = std::max(1u, img.width / 2);
    o.height = std::max(1u, img.height / 2);
    o.rgba.resize((size_t)o.width * o.height * 4);
    for (uint32_t y = 0; y < o.height; y++)
        for (uint32_t x = 0; x < o.width; x++) {
            uint32_t sx = std::min(x * 2, img.width - 1), sy = std::min(y * 2, img.height - 1);
            uint32_t sx1 = std::min(sx + 1, img.width - 1), sy1 = std::min(sy + 1, img.height - 1);
            const uint8_t* a = img.px(sx, sy); const uint8_t* b = img.px(sx1, sy);
            const uint8_t* c = img.px(sx, sy1); const uint8_t* d = img.px(sx1, sy1);
            for (int k = 0; k < 4; k++) o.px(x, y)[k] = (uint8_t)((a[k] + b[k] + c[k] + d[k] + 2) / 4);
        }
    return o;
}

Texture encode_texture(const Image& img, TextureFormat f, const std::string& name, uint32_t levels) {
    if (img.width == 0 || img.height == 0) throw Error("cannot encode an empty image");
    uint32_t max_levels = 1;
    for (uint32_t w = img.width, h = img.height; w > 1 || h > 1; w = std::max(1u, w / 2), h = std::max(1u, h / 2)) max_levels++;
    if (levels == 0 || levels > max_levels) levels = max_levels;
    Texture t;
    t.name = name;
    t.width = (uint16_t)img.width;
    t.height = (uint16_t)img.height;
    t.format = f;
    t.levels = (uint8_t)levels;
    t.stride = (uint16_t)texture_row_pitch(f, img.width);
    t.set_usage(TextureUsage::Default);
    t.usage_data |= (1u << 24) << 5; // the flag nearly every game texture carries
    Image cur = img;
    for (uint32_t l = 0; l < levels; l++) {
        Bytes lv = encode_level(cur, f);
        t.data.insert(t.data.end(), lv.begin(), lv.end());
        if (l + 1 < levels) cur = downsample(cur);
    }
    return t;
}

// ================================================================ DDS

namespace {
constexpr uint32_t fourcc(const char s[5]) { return (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24); }
inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline void wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
}

Texture read_dds(std::span<const uint8_t> file, const std::string& name) {
    if (file.size() < 128 || rd32(file.data()) != fourcc("DDS ")) throw Error("not a DDS file");
    const uint8_t* h = file.data() + 4;
    if (rd32(h) != 124) throw Error("DDS header size is not 124");
    uint32_t flags = rd32(h + 4), height = rd32(h + 8), width = rd32(h + 12);
    uint32_t mips = (flags & 0x20000) ? std::max(1u, rd32(h + 24)) : 1;
    // a texture keeps its size in 16 bits and its level count in a byte
    if (width > 0xFFFF || height > 0xFFFF) throw Error("DDS: " + std::to_string(width) + " x " + std::to_string(height) + " is larger than a texture can be");
    if (mips > 0xFF) throw Error("DDS: " + std::to_string(mips) + " mip levels");
    const uint8_t* pf = h + 72;
    uint32_t pfflags = rd32(pf + 4), cc = rd32(pf + 8), bpp = rd32(pf + 12);
    uint32_t rmask = rd32(pf + 16), gmask = rd32(pf + 20), bmask = rd32(pf + 24), amask = rd32(pf + 28);
    size_t data_off = 128;
    TextureFormat f = TextureFormat::Unknown;
    if (pfflags & 0x4) { // fourCC
        if (cc == fourcc("DX10")) {
            if (file.size() < 148) throw Error("DDS DX10 header truncated");
            uint32_t dxgi = rd32(file.data() + 128);
            data_off = 148;
            switch (dxgi) {
                case 71: case 72: f = TextureFormat::DXT1; break;
                case 74: case 75: f = TextureFormat::DXT3; break;
                case 77: case 78: f = TextureFormat::DXT5; break;
                case 80: f = TextureFormat::ATI1; break;
                case 83: f = TextureFormat::ATI2; break;
                case 98: case 99: f = TextureFormat::BC7; break;
                case 28: case 29: f = TextureFormat::A8B8G8R8; break;
                case 87: case 91: f = TextureFormat::A8R8G8B8; break;
                case 65: f = TextureFormat::A8; break;
                case 61: f = TextureFormat::L8; break;
                default: throw Error("DDS: unsupported DXGI format " + std::to_string(dxgi));
            }
        } else if (cc == fourcc("DXT1")) f = TextureFormat::DXT1;
        else if (cc == fourcc("DXT3")) f = TextureFormat::DXT3;
        else if (cc == fourcc("DXT5")) f = TextureFormat::DXT5;
        else if (cc == fourcc("ATI1") || cc == fourcc("BC4U")) f = TextureFormat::ATI1;
        else if (cc == fourcc("ATI2") || cc == fourcc("BC5U")) f = TextureFormat::ATI2;
        else throw Error("DDS: unsupported fourCC");
    } else if (pfflags & 0x40) { // RGB
        if (bpp == 32 && rmask == 0x00FF0000 && amask == 0xFF000000) f = TextureFormat::A8R8G8B8;
        else if (bpp == 32 && rmask == 0x00FF0000 && amask == 0) f = TextureFormat::X8R8G8B8;
        else if (bpp == 32 && rmask == 0x000000FF) f = TextureFormat::A8B8G8R8;
        else if (bpp == 16 && rmask == 0x7C00) f = TextureFormat::A1R5G5B5;
        else throw Error("DDS: unsupported RGB layout");
    } else if (pfflags & 0x2) { f = TextureFormat::A8; }
    else if (pfflags & 0x20000) { f = TextureFormat::L8; }
    else throw Error("DDS: unsupported pixel format");
    (void)gmask; (void)bmask;
    Texture t;
    t.name = name;
    t.width = (uint16_t)width;
    t.height = (uint16_t)height;
    t.format = f;
    t.levels = (uint8_t)mips;
    t.stride = (uint16_t)texture_row_pitch(f, width);
    size_t need = texture_chain_size(f, width, height, mips);
    if (file.size() < data_off + need) throw Error("DDS: file shorter than its mip chain");
    t.data.assign(file.begin() + (ptrdiff_t)data_off, file.begin() + (ptrdiff_t)(data_off + need));
    t.set_usage(TextureUsage::Default);
    t.usage_data |= (1u << 24) << 5;
    return t;
}

Bytes write_dds(const Texture& t) {
    bool dx10 = t.format == TextureFormat::BC7;
    Bytes out(dx10 ? 148 : 128, 0);
    uint8_t* p = out.data();
    wr32(p, fourcc("DDS "));
    uint8_t* h = p + 4;
    wr32(h, 124);
    uint32_t flags = 0x1 | 0x2 | 0x4 | 0x1000 | (t.levels > 1 ? 0x20000 : 0) | (texture_format_is_block(t.format) ? 0x80000 : 0x8);
    wr32(h + 4, flags);
    wr32(h + 8, t.height);
    wr32(h + 12, t.width);
    wr32(h + 16, texture_format_is_block(t.format) ? (uint32_t)texture_level_size(t.format, t.width, t.height) : texture_row_pitch(t.format, t.width));
    wr32(h + 24, t.levels);
    uint8_t* pf = h + 72;
    wr32(pf, 32);
    switch (t.format) {
        case TextureFormat::DXT1: wr32(pf + 4, 0x4); wr32(pf + 8, fourcc("DXT1")); break;
        case TextureFormat::DXT3: wr32(pf + 4, 0x4); wr32(pf + 8, fourcc("DXT3")); break;
        case TextureFormat::DXT5: wr32(pf + 4, 0x4); wr32(pf + 8, fourcc("DXT5")); break;
        case TextureFormat::ATI1: wr32(pf + 4, 0x4); wr32(pf + 8, fourcc("ATI1")); break;
        case TextureFormat::ATI2: wr32(pf + 4, 0x4); wr32(pf + 8, fourcc("ATI2")); break;
        case TextureFormat::BC7:  wr32(pf + 4, 0x4); wr32(pf + 8, fourcc("DX10")); break;
        case TextureFormat::A8R8G8B8: wr32(pf + 4, 0x41); wr32(pf + 12, 32); wr32(pf + 16, 0x00FF0000); wr32(pf + 20, 0x0000FF00); wr32(pf + 24, 0x000000FF); wr32(pf + 28, 0xFF000000); break;
        case TextureFormat::X8R8G8B8: wr32(pf + 4, 0x40); wr32(pf + 12, 32); wr32(pf + 16, 0x00FF0000); wr32(pf + 20, 0x0000FF00); wr32(pf + 24, 0x000000FF); break;
        case TextureFormat::A8B8G8R8: wr32(pf + 4, 0x41); wr32(pf + 12, 32); wr32(pf + 16, 0x000000FF); wr32(pf + 20, 0x0000FF00); wr32(pf + 24, 0x00FF0000); wr32(pf + 28, 0xFF000000); break;
        case TextureFormat::A1R5G5B5: wr32(pf + 4, 0x41); wr32(pf + 12, 16); wr32(pf + 16, 0x7C00); wr32(pf + 20, 0x03E0); wr32(pf + 24, 0x001F); wr32(pf + 28, 0x8000); break;
        case TextureFormat::A8: wr32(pf + 4, 0x2); wr32(pf + 12, 8); wr32(pf + 28, 0xFF); break;
        case TextureFormat::L8: wr32(pf + 4, 0x20000); wr32(pf + 12, 8); wr32(pf + 16, 0xFF); break;
        default: throw Error("write_dds: unknown texture format");
    }
    wr32(h + 104, 0x1000 | (t.levels > 1 ? 0x400008 : 0)); // caps: texture (+ mipmap, complex)
    if (dx10) {
        wr32(p + 128, 98); // DXGI_FORMAT_BC7_UNORM
        wr32(p + 132, 3);  // texture 2D
        wr32(p + 140, 1);  // array size
    }
    out.insert(out.end(), t.data.begin(), t.data.end());
    return out;
}

// ================================================================ PNG

namespace {
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
void put_be32(Bytes& b, uint32_t v) { b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
void put_chunk(Bytes& out, const char type[5], const Bytes& data) {
    put_be32(out, (uint32_t)data.size());
    Bytes crcbuf(type, type + 4);
    crcbuf.insert(crcbuf.end(), data.begin(), data.end());
    out.insert(out.end(), crcbuf.begin(), crcbuf.end());
    put_be32(out, (uint32_t)mz_crc32(0, crcbuf.data(), crcbuf.size()));
}
uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (uint8_t)((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c));
}
}

Image read_png(std::span<const uint8_t> file) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (file.size() < 33 || std::memcmp(file.data(), sig, 8) != 0) throw Error("not a PNG file");
    size_t pos = 8;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    Bytes idat, plte, trns;
    while (pos + 8 <= file.size()) {
        uint32_t len = be32(&file[pos]);
        std::string type((const char*)&file[pos + 4], 4);
        pos += 8;
        if (pos + len > file.size()) throw Error("PNG chunk truncated");
        const uint8_t* d = &file[pos];
        if (type == "IHDR") {
            if (len < 13) throw Error("PNG: IHDR chunk shorter than 13 bytes");
            w = be32(d); h = be32(d + 4); depth = d[8]; ctype = d[9]; interlace = d[12];
        } else if (type == "IDAT") idat.insert(idat.end(), d, d + len);
        else if (type == "PLTE") plte.assign(d, d + len);
        else if (type == "tRNS") trns.assign(d, d + len);
        else if (type == "IEND") break;
        pos += len + 4;
    }
    if (depth != 8) throw Error("PNG: only 8 bit depth is supported");
    if (interlace) throw Error("PNG: interlaced images are not supported");
    int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!channels) throw Error("PNG: unsupported colour type");
    // the filtered rows the image data must inflate to: never more than the compressed data could give (1032 bytes per
    // byte of deflate), which refuses a made-up size before anything of it is allocated
    const uint64_t raw_size = ((uint64_t)w * (uint64_t)channels + 1) * (uint64_t)h;
    if (w == 0 || h == 0 || w > 0x7FFFFFFF || h > 0x7FFFFFFF || raw_size > (uint64_t)idat.size() * 1032 + 0x10000 || raw_size > UINT32_MAX)
        throw Error("PNG: " + std::to_string(w) + " x " + std::to_string(h) + " does not fit its image data");
    size_t stride = (size_t)w * channels;
    Bytes raw((size_t)raw_size);
    {
        mz_stream zs{};
        if (mz_inflateInit(&zs) != MZ_OK) throw Error("inflateInit failed");
        zs.next_in = idat.data(); zs.avail_in = (unsigned)idat.size();
        zs.next_out = raw.data(); zs.avail_out = (unsigned)raw.size();
        int rc = mz_inflate(&zs, MZ_FINISH);
        mz_inflateEnd(&zs);
        if (rc != MZ_STREAM_END) throw Error("PNG: corrupt image data");
    }
    Bytes prev(stride, 0), cur(stride);
    Image img;
    img.width = w; img.height = h;
    img.rgba.resize((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* line = &raw[(stride + 1) * y];
        uint8_t filter = line[0];
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)channels ? cur[i - channels] : 0;
            int b = prev[i];
            int c = i >= (size_t)channels ? prev[i - channels] : 0;
            int x = line[1 + i];
            switch (filter) {
                case 0: cur[i] = (uint8_t)x; break;
                case 1: cur[i] = (uint8_t)(x + a); break;
                case 2: cur[i] = (uint8_t)(x + b); break;
                case 3: cur[i] = (uint8_t)(x + ((a + b) >> 1)); break;
                case 4: cur[i] = (uint8_t)(x + paeth(a, b, c)); break;
                default: throw Error("PNG: bad filter type");
            }
        }
        for (uint32_t xp = 0; xp < w; xp++) {
            uint8_t* d = img.px(xp, y);
            const uint8_t* s = &cur[(size_t)xp * channels];
            switch (ctype) {
                case 0: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
                case 2: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
                case 3: {
                    size_t i = s[0];
                    if (i * 3 + 2 < plte.size()) { d[0] = plte[i * 3]; d[1] = plte[i * 3 + 1]; d[2] = plte[i * 3 + 2]; }
                    d[3] = i < trns.size() ? trns[i] : 255;
                    break;
                }
                case 4: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
                case 6: std::memcpy(d, s, 4); break;
            }
        }
        std::swap(prev, cur);
    }
    return img;
}

Bytes write_png(const Image& img) {
    Bytes out = {137, 80, 78, 71, 13, 10, 26, 10};
    Bytes ihdr;
    put_be32(ihdr, img.width);
    put_be32(ihdr, img.height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    put_chunk(out, "IHDR", ihdr);
    Bytes raw;
    raw.reserve(((size_t)img.width * 4 + 1) * img.height);
    for (uint32_t y = 0; y < img.height; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), img.px(0, y), img.px(0, y) + (size_t)img.width * 4);
    }
    mz_ulong clen = mz_compressBound((mz_ulong)raw.size());
    Bytes comp(clen);
    if (mz_compress2(comp.data(), &clen, raw.data(), (mz_ulong)raw.size(), 9) != MZ_OK) throw Error("PNG compress failed");
    comp.resize(clen);
    put_chunk(out, "IDAT", comp);
    put_chunk(out, "IEND", {});
    return out;
}

} // namespace rk
