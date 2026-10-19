#include "ragekit/gxt2.h"
#include <algorithm>
#include <cstring>
#include "ragekit/error.h"

namespace rk {

namespace {

constexpr uint32_t kMagic = 0x47585432;   // "2TXG"

uint32_t u32_at(std::span<const uint8_t> b, size_t at) {
    uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

void put32(Bytes& b, uint32_t v) { b.insert(b.end(), (const uint8_t*)&v, (const uint8_t*)&v + 4); }

}  // namespace

const std::string* TextTable::find(uint32_t hash) const {
    for (const Entry& e : entries)
        if (e.hash == hash) return &e.text;
    return nullptr;
}

void TextTable::set(uint32_t hash, std::string text) {
    auto it = std::lower_bound(entries.begin(), entries.end(), hash, [](const Entry& e, uint32_t h) { return e.hash < h; });
    if (it != entries.end() && it->hash == hash) it->text = std::move(text);
    else entries.insert(it, Entry{hash, std::move(text)});
}

bool is_gxt2(std::span<const uint8_t> file) { return file.size() >= 8 && u32_at(file, 0) == kMagic; }

TextTable read_gxt2(std::span<const uint8_t> file) {
    if (!is_gxt2(file)) throw Error("gxt2: not a text table (no 2TXG magic)");
    if (file.size() < 16) throw Error("gxt2: a file of " + std::to_string(file.size()) + " bytes holds no table");
    const uint32_t count = u32_at(file, 4);
    if (count > (file.size() - 16) / 8) throw Error("gxt2: " + std::to_string(count) + " entries do not fit in " + std::to_string(file.size()) + " bytes");
    const size_t table_end = 8 + (size_t)count * 8;
    if (u32_at(file, table_end) != kMagic) throw Error("gxt2: no second 2TXG magic after the entries");
    const uint32_t end = u32_at(file, table_end + 4);
    if (end > file.size() || end < table_end + 8) throw Error("gxt2: the strings end at " + std::to_string(end) + ", outside the file");
    TextTable t;
    t.entries.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        TextTable::Entry& e = t.entries[i];
        e.hash = u32_at(file, 8 + i * 8);
        const uint32_t off = u32_at(file, 12 + i * 8);
        if (off < table_end + 8 || off >= end) throw Error("gxt2: entry " + std::to_string(i) + " starts at " + std::to_string(off) + ", outside the strings");
        const uint8_t* s = file.data() + off;
        const uint8_t* nul = (const uint8_t*)std::memchr(s, 0, end - off);
        if (!nul) throw Error("gxt2: entry " + std::to_string(i) + " has no terminating NUL");
        e.text.assign((const char*)s, (size_t)(nul - s));
    }
    return t;
}

Bytes write_gxt2(const TextTable& t) {
    Bytes b;
    put32(b, kMagic);
    put32(b, (uint32_t)t.entries.size());
    size_t offset = 16 + t.entries.size() * 8;
    for (const TextTable::Entry& e : t.entries) {
        put32(b, e.hash);
        put32(b, (uint32_t)offset);
        offset += e.text.size() + 1;
    }
    if (offset > UINT32_MAX) throw Error("gxt2: the strings pass 4 GB");
    put32(b, kMagic);
    put32(b, (uint32_t)offset);
    for (const TextTable::Entry& e : t.entries) {
        b.insert(b.end(), e.text.begin(), e.text.end());
        b.push_back(0);
    }
    return b;
}

}  // namespace rk
