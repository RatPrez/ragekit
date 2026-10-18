#include "ragekit/hash.h"
#include "ragekit/types.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace rk {

uint32_t joaat(std::string_view s) {
    uint32_t h = 0;
    for (unsigned char c : s) {
        h += c;
        h += (h << 10);
        h ^= (h >> 6);
    }
    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);
    return h;
}

uint32_t joaat_lower(std::string_view s) {
    uint32_t h = 0;
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h += c;
        h += (h << 10);
        h ^= (h >> 6);
    }
    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);
    return h;
}

static uint32_t elf_hash_upper(std::string_view s) {
    uint32_t hash = 0, x = 0;
    for (unsigned char c : s) {
        if ((unsigned char)(c - 'a') <= 25u) c = (unsigned char)(c - 32);
        hash = (hash << 4) + c;
        if ((x = hash & 0xF0000000u) != 0) hash ^= (x >> 24);
        hash &= ~x;
    }
    return hash;
}

uint16_t bone_tag(std::string_view bone_name) {
    return (uint16_t)(elf_hash_upper(bone_name) % 0xFE8F + 0x170);
}

std::string hash_name(uint32_t h) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "hash_%08X", h);
    return buf;
}

uint32_t hash_or_name(std::string_view s) {
    if (s.empty()) return 0;
    // "hash_XXXXXXXX" and "0x..." only when every character after the prefix is a hex digit: "hash_lamppost" is a name
    auto all_hex = [](std::string_view t) {
        if (t.empty()) return false;
        for (char c : t) if (!std::isxdigit((unsigned char)c)) return false;
        return true;
    };
    if (s.size() == 13 && s.substr(0, 5) == "hash_" && all_hex(s.substr(5)))
        return (uint32_t)std::strtoul(std::string(s.substr(5)).c_str(), nullptr, 16);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && all_hex(s.substr(2)))
        return (uint32_t)std::strtoul(std::string(s.substr(2)).c_str(), nullptr, 16);
    bool digits = true;
    for (char c : s) if (!std::isdigit((unsigned char)c)) { digits = false; break; }
    if (digits) return (uint32_t)std::strtoul(std::string(s).c_str(), nullptr, 10);
    return joaat_lower(s);
}

float half_to_float(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) f = sign << 31;
        else {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            f = (sign << 31) | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | 0x7F800000 | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, 4);
    return out;
}

uint16_t float_to_half(float v) {
    uint32_t f;
    std::memcpy(&f, &v, 4);
    uint32_t sign = (f >> 16) & 0x8000;
    int32_t exp = (int32_t)((f >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = f & 0x7FFFFF;
    if (((f >> 23) & 0xFF) == 0xFF) return (uint16_t)(sign | 0x7C00 | (mant ? 0x200 : 0));
    if (exp >= 31) return (uint16_t)(sign | 0x7C00);
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t hm = mant >> shift;
        uint32_t rem = mant & ((1u << shift) - 1);
        uint32_t half = 1u << (shift - 1);
        if (rem > half || (rem == half && (hm & 1))) hm++;
        return (uint16_t)(sign | hm);
    }
    uint32_t hm = mant >> 13;
    uint32_t rem = mant & 0x1FFF;
    uint16_t h = (uint16_t)(sign | ((uint32_t)exp << 10) | hm);
    if (rem > 0x1000 || (rem == 0x1000 && (hm & 1))) h++;
    return h;
}

} // namespace rk
