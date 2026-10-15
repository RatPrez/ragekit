#include "ragekit/texture.h"
#include "ragekit/hash.h"
#include "ragekit/error.h"
#include "texture_io.h"
#include <algorithm>
#include <cstring>

namespace rk {

// ---------------------------------------------------------------- formats

const char* texture_format_name(TextureFormat f) {
    switch (f) {
        case TextureFormat::A8R8G8B8: return "D3DFMT_A8R8G8B8";
        case TextureFormat::X8R8G8B8: return "D3DFMT_X8R8G8B8";
        case TextureFormat::A1R5G5B5: return "D3DFMT_A1R5G5B5";
        case TextureFormat::A8:       return "D3DFMT_A8";
        case TextureFormat::A8B8G8R8: return "D3DFMT_A8B8G8R8";
        case TextureFormat::L8:       return "D3DFMT_L8";
        case TextureFormat::DXT1:     return "D3DFMT_DXT1";
        case TextureFormat::DXT3:     return "D3DFMT_DXT3";
        case TextureFormat::DXT5:     return "D3DFMT_DXT5";
        case TextureFormat::ATI1:     return "D3DFMT_ATI1";
        case TextureFormat::ATI2:     return "D3DFMT_ATI2";
        case TextureFormat::BC7:      return "D3DFMT_BC7";
        default: return "UNKNOWN";
    }
}

TextureFormat texture_format_from_name(std::string_view n) {
    static const TextureFormat all[] = {
        TextureFormat::A8R8G8B8, TextureFormat::X8R8G8B8, TextureFormat::A1R5G5B5, TextureFormat::A8,
        TextureFormat::A8B8G8R8, TextureFormat::L8, TextureFormat::DXT1, TextureFormat::DXT3,
        TextureFormat::DXT5, TextureFormat::ATI1, TextureFormat::ATI2, TextureFormat::BC7};
    for (auto f : all) {
        std::string_view name = texture_format_name(f);
        if (n == name || (name.size() > 7 && n == name.substr(7))) return f;
    }
    return TextureFormat::Unknown;
}

bool texture_format_is_block(TextureFormat f) {
    switch (f) {
        case TextureFormat::DXT1: case TextureFormat::DXT3: case TextureFormat::DXT5:
        case TextureFormat::ATI1: case TextureFormat::ATI2: case TextureFormat::BC7: return true;
        default: return false;
    }
}

uint32_t texture_format_unit_bytes(TextureFormat f) {
    switch (f) {
        case TextureFormat::DXT1: case TextureFormat::ATI1: return 8;
        case TextureFormat::DXT3: case TextureFormat::DXT5: case TextureFormat::ATI2: case TextureFormat::BC7: return 16;
        case TextureFormat::A8R8G8B8: case TextureFormat::X8R8G8B8: case TextureFormat::A8B8G8R8: return 4;
        case TextureFormat::A1R5G5B5: return 2;
        case TextureFormat::A8: case TextureFormat::L8: return 1;
        default: return 0;
    }
}

size_t texture_level_size(TextureFormat f, uint32_t w, uint32_t h) {
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (texture_format_is_block(f))
        return (size_t)((w + 3) / 4) * ((h + 3) / 4) * texture_format_unit_bytes(f);
    return (size_t)w * h * texture_format_unit_bytes(f);
}

uint32_t texture_row_pitch(TextureFormat f, uint32_t w) {
    if (w == 0) w = 1;
    if (texture_format_is_block(f)) return ((w + 3) / 4) * texture_format_unit_bytes(f);
    return w * texture_format_unit_bytes(f);
}

size_t texture_chain_size(TextureFormat f, uint32_t w, uint32_t h, uint32_t levels, uint32_t depth) {
    size_t n = 0;
    for (uint32_t i = 0; i < levels; i++) {
        n += texture_level_size(f, w, h);
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    return n * std::max(1u, depth);
}

uint32_t Texture::name_hash() const { return joaat_lower(name); }

std::span<const uint8_t> Texture::level(uint32_t lvl, uint32_t& w, uint32_t& h) const {
    w = width; h = height;
    size_t off = 0;
    for (uint32_t i = 0; i < lvl && i < levels; i++) {
        off += texture_level_size(format, w, h);
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    size_t n = texture_level_size(format, w, h);
    if (off + n > data.size()) n = off < data.size() ? data.size() - off : 0;
    return std::span<const uint8_t>(data.data() + off, n);
}

const Texture* TextureDictionary::find(uint32_t h) const {
    for (auto& t : textures) if (t.name_hash() == h) return &t;
    return nullptr;
}
const Texture* TextureDictionary::find(std::string_view name) const { return find(joaat_lower(name)); }

void TextureDictionary::add(Texture t) {
    uint32_t h = t.name_hash();
    for (auto& e : textures) if (e.name_hash() == h) { e = std::move(t); return; }
    textures.push_back(std::move(t));
    sort();
}

void TextureDictionary::sort() {
    std::stable_sort(textures.begin(), textures.end(),
                     [](const Texture& a, const Texture& b) { return a.name_hash() < b.name_hash(); });
}

// ---------------------------------------------------------------- reading

Texture read_texture(const Reader& r, uint64_t ptr) {
    Texture t;
    auto c = r.cursor(ptr);
    c.u32(); // VFT
    c.u32(); // 1
    c.skip(32); // 8 dwords, zero
    uint64_t name_ptr = c.u64(); // 0x28
    c.u16();                     // 0x30: 1
    t.unk32 = c.u16();           // Unknown_32h
    c.skip(12);                  // 34h 38h 3Ch
    t.usage_data = c.u32();      // 0x40
    c.u32();                     // 0x44
    t.extra_flags = c.u32();     // 0x48
    c.u32();                     // 0x4C
    t.width = c.u16();           // 0x50
    t.height = c.u16();
    t.depth = c.u16();
    t.stride = c.u16();
    t.format = (TextureFormat)c.u32(); // 0x58
    c.u8();                      // 0x5C
    t.levels = c.u8();           // 0x5D
    c.u16();                     // 0x5E
    c.skip(16);                  // 0x60..0x6F
    uint64_t data_ptr = c.u64(); // 0x70
    t.name = r.str(name_ptr);
    if (data_ptr != 0) {
        size_t want = texture_chain_size(t.format, t.width, t.height, t.levels, t.depth);
        size_t have = r.remaining(data_ptr);
        if (want > have) want = have; // some files are cut short in their last mips
        t.data = r.bytes(data_ptr, want);
    }
    return t;
}

std::string read_texture_ref(const Reader& r, uint64_t ptr) {
    if (ptr == 0) return "";
    uint64_t name_ptr = r.u64(ptr + 0x28);
    return r.str(name_ptr);
}

TextureDictionary read_txd(const Reader& r, uint64_t ptr) {
    TextureDictionary d;
    auto c = r.cursor(ptr);
    c.u32(); c.u32(); c.u64();  // VFT, 1, pages info
    c.skip(16);                 // 10h..1Fh
    uint64_t hashes_ptr = c.u64();
    uint16_t hashes_count = c.u16();
    c.u16(); c.u32();           // capacity, pad
    uint64_t tex_ptr = c.u64();
    uint16_t tex_count = c.u16();
    auto hashes = r.array<uint32_t>(hashes_ptr, hashes_count);
    auto ptrs = r.array<uint64_t>(tex_ptr, tex_count);
    for (size_t i = 0; i < ptrs.size(); i++) {
        if (ptrs[i] == 0) continue;
        Texture t = read_texture(r, ptrs[i]);
        if (t.name.empty() && i < hashes.size()) t.name = hash_name(hashes[i]);
        d.textures.push_back(std::move(t));
    }
    return d;
}

TextureDictionary read_ytd(std::span<const uint8_t> file) {
    rsc::Resource res = rsc::Resource::load(file);
    Reader r(res);
    return read_txd(r, rsc::kSystemBase);
}

// ---------------------------------------------------------------- writing

// Virtual table pointers as they appear in game files. The game overwrites them on
// load, so any value works; these keep diffs against game files small.
static constexpr uint32_t kTextureVFT = 0x405A0B18;
static constexpr uint32_t kTxdVFT     = 0x40589E78;

BlockId write_texture(Builder& b, const Texture& t) {
    BlockId blk = b.alloc(Segment::System, 144);
    BlockId name = t.name.empty() ? BlockId{} : b.string(t.name);
    BlockId data = b.array(Segment::Graphics, t.data, 16);
    uint16_t stride = t.stride ? t.stride : (uint16_t)texture_row_pitch(t.format, t.width);
    BlockWriter w(b, blk);
    w.u32(kTextureVFT);
    w.u32(1);
    w.zero(32);
    w.ptr(name);
    w.u16(1);
    w.u16(t.unk32);
    w.zero(12);
    w.u32(t.usage_data);
    w.u32(0);
    w.u32(t.extra_flags);
    w.u32(0);
    w.u16(t.width);
    w.u16(t.height);
    w.u16(t.depth);
    w.u16(stride);
    w.u32((uint32_t)t.format);
    w.u8(0);
    w.u8(t.levels);
    w.u16(0);
    w.zero(16);
    w.ptr(data);
    w.zero(24);
    return blk;
}

BlockId write_texture_ref(Builder& b, const std::string& name) {
    BlockId blk = b.alloc(Segment::System, 80);
    BlockId nameb = name.empty() ? BlockId{} : b.string(name);
    BlockWriter w(b, blk);
    w.u32(kTextureVFT);
    w.u32(1);
    w.zero(32);
    w.ptr(nameb);
    w.u16(1);
    w.u16(2);     // stub textures carry 2 here
    w.zero(28);
    return blk;
}

BlockId write_txd(Builder& b, const TextureDictionary& txd_in, TextureBlocks* blocks, bool root) {
    TextureDictionary txd = txd_in;
    txd.sort();
    BlockId blk = b.alloc(Segment::System, 64);
    BlockId pages = root ? b.alloc_pages_info() : BlockId{};
    std::vector<uint32_t> hashes;
    std::vector<BlockId> texblocks;
    for (auto& t : txd.textures) {
        hashes.push_back(t.name_hash());
        BlockId tb = write_texture(b, t);
        texblocks.push_back(tb);
        if (blocks) (*blocks)[t.name_hash()] = tb;
    }
    BlockId hashes_blk = b.array(Segment::System, hashes);
    BlockId ptrs_blk;
    if (!texblocks.empty()) {
        ptrs_blk = b.alloc(Segment::System, texblocks.size() * 8);
        for (size_t i = 0; i < texblocks.size(); i++) b.pointer(ptrs_blk, i * 8, texblocks[i]);
    }
    BlockWriter w(b, blk);
    w.u32(kTxdVFT);
    w.u32(1);
    w.ptr(pages);
    w.u32(0); w.u32(0); w.u32(1); w.u32(0);
    w.ptr(hashes_blk);
    w.u16((uint16_t)hashes.size());
    w.u16((uint16_t)hashes.size());
    w.u32(0);
    w.ptr(ptrs_blk);
    w.u16((uint16_t)texblocks.size());
    w.u16((uint16_t)texblocks.size());
    w.u32(0);
    return blk;
}

Bytes write_ytd(const TextureDictionary& txd) {
    Builder b;
    write_txd(b, txd, nullptr, true);
    return b.build(rsc::kYtd);
}

} // namespace rk
