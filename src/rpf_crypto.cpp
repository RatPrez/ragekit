#include "rpf_crypto.h"
#include <array>
#include <cstring>

namespace rk::rpfcrypto {

// ---- SHA1 -------------------------------------------------------------------------------

static inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t full = len / 64;
    for (size_t i = 0; i < full; i++) sha1_block(h, data + 64 * i);
    uint8_t tail[128] = {};
    size_t rem = len - full * 64;
    std::memcpy(tail, data + full * 64, rem);
    tail[rem] = 0x80;
    size_t tail_len = rem + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < tail_len; i += 64) sha1_block(h, tail + i);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i];
    }
}

// ---- AES-256 ----------------------------------------------------------------------------

namespace {
struct AesTables {
    uint8_t sbox[256], inv[256];
    uint8_t m2[256], m3[256], m9[256], m11[256], m13[256], m14[256];
    AesTables() {
        // S-box from the multiplicative inverse in GF(2^8) plus the affine transform.
        uint8_t p = 1, q = 1;
        do {
            p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
            if (q & 0x80) q ^= 0x09;
            uint8_t x = (uint8_t)(q ^ rol8(q, 1) ^ rol8(q, 2) ^ rol8(q, 3) ^ rol8(q, 4));
            sbox[p] = (uint8_t)(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; i++) inv[sbox[i]] = (uint8_t)i;
        for (int i = 0; i < 256; i++) {
            m2[i] = mul((uint8_t)i, 2); m3[i] = mul((uint8_t)i, 3); m9[i] = mul((uint8_t)i, 9);
            m11[i] = mul((uint8_t)i, 11); m13[i] = mul((uint8_t)i, 13); m14[i] = mul((uint8_t)i, 14);
        }
    }
    static uint8_t rol8(uint8_t v, int n) { return (uint8_t)((v << n) | (v >> (8 - n))); }
    static uint8_t mul(uint8_t a, uint8_t b) {
        uint8_t r = 0;
        while (b) {
            if (b & 1) r ^= a;
            a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
            b >>= 1;
        }
        return r;
    }
};
const AesTables& T() { static const AesTables t; return t; }

struct AesKey { uint8_t rk[15][16]; };

AesKey expand(const uint8_t key[32]) {
    const auto& t = T();
    uint8_t w[240];
    std::memcpy(w, key, 32);
    uint8_t rcon = 1;
    for (int i = 8; i < 60; i++) {
        uint8_t tmp[4];
        std::memcpy(tmp, w + 4 * (i - 1), 4);
        if (i % 8 == 0) {
            uint8_t r = tmp[0];
            tmp[0] = (uint8_t)(t.sbox[tmp[1]] ^ rcon); tmp[1] = t.sbox[tmp[2]]; tmp[2] = t.sbox[tmp[3]]; tmp[3] = t.sbox[r];
            rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
        } else if (i % 8 == 4) {
            for (int j = 0; j < 4; j++) tmp[j] = t.sbox[tmp[j]];
        }
        for (int j = 0; j < 4; j++) w[4 * i + j] = (uint8_t)(w[4 * (i - 8) + j] ^ tmp[j]);
    }
    AesKey k;
    std::memcpy(k.rk, w, 240);
    return k;
}

void add_round_key(uint8_t* s, const uint8_t* k) { for (int i = 0; i < 16; i++) s[i] ^= k[i]; }

void dec_block(uint8_t* s, const AesKey& k) {
    const auto& t = T();
    add_round_key(s, k.rk[14]);
    for (int round = 13; round >= 0; round--) {
        // inverse shift rows
        uint8_t o[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) o[4 * ((c + r) % 4) + r] = s[4 * c + r];
        for (int i = 0; i < 16; i++) s[i] = t.inv[o[i]];
        add_round_key(s, k.rk[round]);
        if (round > 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
                s[4 * c]     = (uint8_t)(t.m14[a0] ^ t.m11[a1] ^ t.m13[a2] ^ t.m9[a3]);
                s[4 * c + 1] = (uint8_t)(t.m9[a0] ^ t.m14[a1] ^ t.m11[a2] ^ t.m13[a3]);
                s[4 * c + 2] = (uint8_t)(t.m13[a0] ^ t.m9[a1] ^ t.m14[a2] ^ t.m11[a3]);
                s[4 * c + 3] = (uint8_t)(t.m11[a0] ^ t.m13[a1] ^ t.m9[a2] ^ t.m14[a3]);
            }
        }
    }
}

void enc_block(uint8_t* s, const AesKey& k) {
    const auto& t = T();
    add_round_key(s, k.rk[0]);
    for (int round = 1; round <= 14; round++) {
        uint8_t o[16];
        for (int i = 0; i < 16; i++) o[i] = t.sbox[s[i]];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) s[4 * c + r] = o[4 * ((c + r) % 4) + r];
        if (round < 14) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
                s[4 * c]     = (uint8_t)(t.m2[a0] ^ t.m3[a1] ^ a2 ^ a3);
                s[4 * c + 1] = (uint8_t)(a0 ^ t.m2[a1] ^ t.m3[a2] ^ a3);
                s[4 * c + 2] = (uint8_t)(a0 ^ a1 ^ t.m2[a2] ^ t.m3[a3]);
                s[4 * c + 3] = (uint8_t)(t.m3[a0] ^ a1 ^ a2 ^ t.m2[a3]);
            }
        }
        add_round_key(s, k.rk[round]);
    }
}
} // namespace

void aes_decrypt(uint8_t* data, size_t len, const uint8_t key[32]) {
    AesKey k = expand(key);
    for (size_t i = 0; i + 16 <= len; i += 16) dec_block(data + i, k);
}
void aes_encrypt(uint8_t* data, size_t len, const uint8_t key[32]) {
    AesKey k = expand(key);
    for (size_t i = 0; i + 16 <= len; i += 16) enc_block(data + i, k);
}

// ---- NG ---------------------------------------------------------------------------------

uint32_t name_hash(std::string_view name, const uint8_t* lut) {
    uint32_t result = 0;
    for (unsigned char c : name) {
        uint32_t temp = 1025u * ((uint32_t)lut[c] + result);
        result = (temp >> 6) ^ temp;
    }
    return 32769u * (((9u * result) >> 11) ^ (9u * result));
}

static inline uint32_t ld32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static inline void st32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

static void ng_block(uint8_t* b, const uint8_t* key, const uint32_t* tables) {
    auto tab = [&](int round, int i) { return tables + ((size_t)round * 16 + i) * 256; };
    auto kw = [&](int round, int i) { return ld32(key + round * 16 + 4 * i); };
    uint8_t d[16], n[16];
    std::memcpy(d, b, 16);
    // rounds 0, 1 and 16: each output word comes from four consecutive input bytes
    auto roundA = [&](int round) {
        for (int w = 0; w < 4; w++) {
            uint32_t x = tab(round, 4 * w)[d[4 * w]] ^ tab(round, 4 * w + 1)[d[4 * w + 1]] ^ tab(round, 4 * w + 2)[d[4 * w + 2]] ^
                         tab(round, 4 * w + 3)[d[4 * w + 3]] ^ kw(round, w);
            st32(n + 4 * w, x);
        }
        std::memcpy(d, n, 16);
    };
    // rounds 2..15: columns mix bytes across the block
    auto roundB = [&](int round) {
        static const int idx[4][4] = {{0, 7, 10, 13}, {1, 4, 11, 14}, {2, 5, 8, 15}, {3, 6, 9, 12}};
        for (int w = 0; w < 4; w++) {
            uint32_t x = tab(round, idx[w][0])[d[idx[w][0]]] ^ tab(round, idx[w][1])[d[idx[w][1]]] ^
                         tab(round, idx[w][2])[d[idx[w][2]]] ^ tab(round, idx[w][3])[d[idx[w][3]]] ^ kw(round, w);
            st32(n + 4 * w, x);
        }
        std::memcpy(d, n, 16);
    };
    roundA(0);
    roundA(1);
    for (int k = 2; k <= 15; k++) roundB(k);
    roundA(16);
    std::memcpy(b, d, 16);
}

void ng_decrypt(uint8_t* data, size_t len, std::string_view name, uint32_t length, const uint8_t* keys, const uint32_t* tables,
                const uint8_t* lut) {
    uint32_t hash = name_hash(name, lut);
    uint32_t keyidx = (hash + length + (101 - 40)) % 0x65;
    const uint8_t* key = keys + (size_t)keyidx * 272;
    for (size_t i = 0; i + 16 <= len; i += 16) ng_block(data + i, key, tables);
}

} // namespace rk::rpfcrypto
