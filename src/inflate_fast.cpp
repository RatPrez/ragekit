// A raw-deflate decoder (RFC 1951) for the common case: a whole, well-formed stream into a buffer that holds it. It reads
// 64 bits at a time and decodes Huffman codes up to 11 (literal / length) or 9 (distance) bits long with one table
// lookup whose entry also carries the length or distance base and its extra bits; longer codes are decoded canonically.
// Anything unusual is refused rather than handled, and the caller falls back on miniz: so a stream it accepts is one
// every inflater decodes the same, and a stream it refuses gives what it always gave.
#include "inflate_fast.h"
#include <cstring>

namespace rk::detail {

namespace {

constexpr int kMaxBits = 15;

// A table entry: bits 0-3 the code's length (0: a longer code, decoded canonically), 4-5 its kind, 8-11 the extra bits
// that follow it, 16-31 the literal or the length / distance base.
enum Kind : uint32_t { kLiteral = 0, kCopy = 1, kEnd = 2, kInvalid = 3 };

const uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// What a symbol means, as the low bits of an entry (its code length added by the table).
uint32_t meaning(uint32_t sym, bool distance) {
    if (distance) return sym < 30 ? (uint32_t)kDistBase[sym] << 16 | (uint32_t)kDistExtra[sym] << 8 | kCopy << 4 : kInvalid << 4;
    if (sym < 256) return sym << 16 | kLiteral << 4;
    if (sym == 256) return kEnd << 4;
    if (sym < 286) return (uint32_t)kLengthBase[sym - 257] << 16 | (uint32_t)kLengthExtra[sym - 257] << 8 | kCopy << 4;
    return kInvalid << 4;
}

struct Huffman {
    int table_bits = 0;
    bool distance = false;
    bool empty = true;
    uint32_t table[1 << 11];
    uint16_t count[kMaxBits + 1];
    uint16_t symbol[288];        // by length, then by symbol (canonical order)

    // False for an over-subscribed set and for an incomplete one other than a single code of one bit (the one case the
    // format allows, for a distance tree); an empty set builds (decoding from it then fails).
    bool build(const uint8_t* lens, int n, int bits, bool dist) {
        table_bits = bits;
        distance = dist;
        std::memset(count, 0, sizeof count);
        for (int i = 0; i < n; i++) count[lens[i]]++;
        count[0] = 0;
        int total = 0;
        for (int l = 1; l <= kMaxBits; l++) total += count[l];
        empty = total == 0;
        std::memset(table, 0, sizeof(uint32_t) << bits);
        if (empty) return true;
        int left = 1;
        for (int l = 1; l <= kMaxBits; l++) {
            left = (left << 1) - count[l];
            if (left < 0) return false;
        }
        if (left > 0 && !(total == 1 && count[1] == 1)) return false;
        uint16_t offs[kMaxBits + 2];
        offs[1] = 0;
        for (int l = 1; l <= kMaxBits; l++) offs[l + 1] = (uint16_t)(offs[l] + count[l]);
        for (int s = 0; s < n; s++)
            if (lens[s]) symbol[offs[lens[s]]++] = (uint16_t)s;
        // canonical codes in symbol order, written bit-reversed (the stream gives a code's first bit first)
        uint32_t code = 0;
        int k = 0;
        for (int l = 1; l <= kMaxBits; l++) {
            for (int c = 0; c < count[l]; c++, k++, code++) {
                if (l > bits) continue;
                uint32_t rev = 0;
                for (int b = 0; b < l; b++) rev |= ((code >> b) & 1) << (l - 1 - b);
                const uint32_t e = meaning(symbol[k], dist) | (uint32_t)l;
                for (uint32_t f = rev; f < (1u << bits); f += 1u << l) table[f] = e;
            }
            code <<= 1;
        }
        return true;
    }

    // A code longer than the table, canonically (puff's decode): the entry it would have, or 0 when the bits name no code.
    uint32_t slow(uint64_t bits) const {
        int code = 0, first = 0, index = 0;
        for (unsigned len = 1; len <= kMaxBits; len++) {
            code |= (int)(bits & 1);
            bits >>= 1;
            const int c = count[len];
            if (code - c < first) return meaning(symbol[index + (code - first)], distance) | len;
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        return 0;
    }
};

struct Bits {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf = 0;   // bits past `n` are the next input bytes or zero, never anything else
    unsigned n = 0;

    // At least 56 bits while 8 bytes of input remain (a whole length / distance pair), else what is left.
    void refill() {
        if (end - p >= 8) {
            uint64_t w;
            std::memcpy(&w, p, 8);
            buf |= w << n;
            p += (63 - n) >> 3;
            n |= 56;
        } else {
            while (n <= 56 && p < end) { buf |= (uint64_t)*p++ << n; n += 8; }
        }
    }
    bool take(unsigned k, uint32_t& v) {
        if (k > n) return false;
        v = (uint32_t)(buf & ((1ull << k) - 1));
        buf >>= k;
        n -= k;
        return true;
    }
    // The entry of the next code, consumed; 0 when the bits are not there or name no code.
    uint32_t entry(const Huffman& h) {
        if (h.empty) return 0;
        uint32_t e = h.table[buf & ((1u << h.table_bits) - 1)];
        if (!e) e = h.slow(buf);
        const unsigned len = e & 15;
        if (!e || len > n) return 0;
        buf >>= len;
        n -= len;
        return e;
    }
};

struct Fixed {
    Huffman lit, dist;
    Fixed() {
        uint8_t l[288];
        for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        lit.build(l, 288, 11, false);
        uint8_t d[32];
        for (auto& x : d) x = 5;
        dist.build(d, 32, 9, true);   // all 32 codes make the set complete; 30 and 31 are invalid entries
    }
};

bool read_dynamic(Bits& b, Huffman& lit, Huffman& dist) {
    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    b.refill();
    uint32_t hlit, hdist, hclen;
    if (!b.take(5, hlit) || !b.take(5, hdist) || !b.take(4, hclen)) return false;
    hlit += 257; hdist += 1; hclen += 4;
    if (hlit > 286 || hdist > 30) return false;
    uint8_t cl[19] = {};
    for (uint32_t i = 0; i < hclen; i++) {
        b.refill();
        uint32_t v;
        if (!b.take(3, v)) return false;
        cl[kOrder[i]] = (uint8_t)v;
    }
    Huffman clh;
    if (!clh.build(cl, 19, 7, false) || clh.empty) return false;
    uint8_t lens[286 + 30] = {};
    uint32_t i = 0;
    while (i < hlit + hdist) {
        b.refill();
        const uint32_t e = b.entry(clh);   // code length symbols 0..18 are literal entries here
        if (!e || ((e >> 4) & 3) != kLiteral) return false;
        const uint32_t sym = e >> 16;
        if (sym < 16) { lens[i++] = (uint8_t)sym; continue; }
        uint32_t rep;
        uint8_t value = 0;
        if (sym == 16) {
            if (i == 0 || !b.take(2, rep)) return false;
            value = lens[i - 1];
            rep += 3;
        } else if (sym == 17) {
            if (!b.take(3, rep)) return false;
            rep += 3;
        } else {
            if (!b.take(7, rep)) return false;
            rep += 11;
        }
        if (i + rep > hlit + hdist) return false;
        while (rep--) lens[i++] = value;
    }
    if (lens[256] == 0) return false;   // a block that cannot end
    return lit.build(lens, (int)hlit, 11, false) && dist.build(lens + hlit, (int)hdist, 9, true);
}

}  // namespace

bool inflate_fast(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap, size_t& out_len) {
    static const Fixed fixed;
    Bits b{in, in + in_len};
    uint8_t* o = out;
    uint8_t* const oend = out + out_cap;
    Huffman lit_dyn, dist_dyn;
    for (;;) {
        b.refill();
        uint32_t final_block, type;
        if (!b.take(1, final_block) || !b.take(2, type)) return false;
        if (type == 0) {
            // stored: the rest of the byte is skipped, the whole bytes still in the buffer go back to the input
            const unsigned skip = b.n & 7;
            b.buf >>= skip;
            b.n -= skip;
            b.p -= b.n >> 3;
            b.buf = 0;
            b.n = 0;
            if (b.end - b.p < 4) return false;
            const uint32_t len = (uint32_t)b.p[0] | (uint32_t)b.p[1] << 8, nlen = (uint32_t)b.p[2] | (uint32_t)b.p[3] << 8;
            b.p += 4;
            if ((len ^ 0xFFFF) != nlen || (size_t)(b.end - b.p) < len || (size_t)(oend - o) < len) return false;
            std::memcpy(o, b.p, len);
            o += len;
            b.p += len;
        } else if (type == 1 || type == 2) {
            const Huffman* lit = &fixed.lit;
            const Huffman* dist = &fixed.dist;
            if (type == 2) {
                if (!read_dynamic(b, lit_dyn, dist_dyn)) return false;
                lit = &lit_dyn;
                dist = &dist_dyn;
            }
            const uint32_t lmask = (1u << lit->table_bits) - 1, dmask = (1u << dist->table_bits) - 1;
            for (;;) {
                // the common case: 8 bytes of input and room for the longest copy and its 8-byte overrun, so neither the
                // bits (56 after a refill; a length / distance pair takes at most 48) nor the output need checking
                if (b.end - b.p >= 8 && (size_t)(oend - o) >= 258 + 8) {
                    b.refill();
                    uint32_t e = lit->table[b.buf & lmask];
                    if (!e && !(e = lit->slow(b.buf))) return false;
                    b.buf >>= e & 15;
                    b.n -= e & 15;
                    uint32_t kind = (e >> 4) & 3;
                    if (kind == kLiteral) {
                        *o++ = (uint8_t)(e >> 16);
                        // a second literal from the same refill: 56 - 15 bits are left
                        e = lit->table[b.buf & lmask];
                        if (e && ((e >> 4) & 3) == kLiteral) {
                            b.buf >>= e & 15;
                            b.n -= e & 15;
                            *o++ = (uint8_t)(e >> 16);
                        }
                        continue;
                    }
                    if (kind == kEnd) break;
                    if (kind != kCopy) return false;
                    unsigned eb = (e >> 8) & 15;
                    const uint32_t length = (e >> 16) + (uint32_t)(b.buf & ((1u << eb) - 1));
                    b.buf >>= eb;
                    b.n -= eb;
                    uint32_t d = dist->table[b.buf & dmask];
                    if (!d && !(d = dist->slow(b.buf))) return false;
                    if (((d >> 4) & 3) != kCopy) return false;
                    b.buf >>= d & 15;
                    b.n -= d & 15;
                    eb = (d >> 8) & 15;
                    const uint32_t dist_v = (d >> 16) + (uint32_t)(b.buf & ((1u << eb) - 1));
                    b.buf >>= eb;
                    b.n -= eb;
                    if (dist_v > (size_t)(o - out)) return false;
                    const uint8_t* src = o - dist_v;
                    uint8_t* const stop = o + length;
                    if (dist_v >= 8) {
                        do { std::memcpy(o, src, 8); o += 8; src += 8; } while (o < stop);
                    } else if (dist_v == 1) {
                        std::memset(o, *src, length);
                    } else {
                        for (; o < stop; o++, src++) *o = *src;
                    }
                    o = stop;
                    continue;
                }
                b.refill();
                const uint32_t e = b.entry(*lit);
                if (!e) return false;
                const uint32_t kind = (e >> 4) & 3;
                if (kind == kLiteral) {
                    if (o == oend) return false;
                    *o++ = (uint8_t)(e >> 16);
                    continue;
                }
                if (kind == kEnd) break;
                if (kind != kCopy) return false;
                uint32_t extra, dextra;
                if (!b.take((e >> 8) & 15, extra)) return false;
                const uint32_t length = (e >> 16) + extra;
                const uint32_t d = b.entry(*dist);
                if (!d || ((d >> 4) & 3) != kCopy || !b.take((d >> 8) & 15, dextra)) return false;
                const uint32_t dist_v = (d >> 16) + dextra;
                if (dist_v > (size_t)(o - out) || length > (size_t)(oend - o)) return false;
                const uint8_t* src = o - dist_v;
                if (dist_v >= 8 && (size_t)(oend - o) >= length + 8) {
                    // 8 bytes at a time: a source at least 8 behind is never one the same step writes
                    uint8_t* const stop = o + length;
                    do { std::memcpy(o, src, 8); o += 8; src += 8; } while (o < stop);
                    o = stop;
                } else if (dist_v == 1) {
                    std::memset(o, *src, length);
                    o += length;
                } else {
                    for (uint32_t k = 0; k < length; k++) o[k] = src[k];
                    o += length;
                }
            }
        } else {
            return false;
        }
        if (final_block) break;
    }
    out_len = (size_t)(o - out);
    return true;
}

}  // namespace rk::detail
