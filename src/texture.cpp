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

uint8_t texture_format_gen9(TextureFormat f) {
    switch (f) {
        case TextureFormat::A8R8G8B8: case TextureFormat::X8R8G8B8: return 0x57;   // B8G8R8A8_UNORM
        case TextureFormat::A1R5G5B5: return 0x56;                                 // B5G5R5A1_UNORM
        case TextureFormat::A8:       return 0x41;                                 // A8_UNORM
        case TextureFormat::A8B8G8R8: return 0x1C;                                 // R8G8B8A8_UNORM
        case TextureFormat::L8:       return 0x3D;                                 // R8_UNORM
        case TextureFormat::DXT1:     return 0x47;                                 // BC1_UNORM
        case TextureFormat::DXT3:     return 0x4A;                                 // BC2_UNORM
        case TextureFormat::DXT5:     return 0x4D;                                 // BC3_UNORM
        case TextureFormat::ATI1:     return 0x50;                                 // BC4_UNORM
        case TextureFormat::ATI2:     return 0x53;                                 // BC5_UNORM
        case TextureFormat::BC7:      return 0x62;                                 // BC7_UNORM
        default: return 0;
    }
}

TextureFormat texture_format_from_gen9(uint8_t code) {
    switch (code) {
        case 0x57: case 0x5A: case 0x5B: return TextureFormat::A8R8G8B8;
        case 0x56: return TextureFormat::A1R5G5B5;
        case 0x41: return TextureFormat::A8;
        case 0x1B: case 0x1C: case 0x1D: return TextureFormat::A8B8G8R8;
        case 0x3C: case 0x3D: return TextureFormat::L8;
        case 0x46: case 0x47: case 0x48: return TextureFormat::DXT1;
        case 0x49: case 0x4A: case 0x4B: return TextureFormat::DXT3;
        case 0x4C: case 0x4D: case 0x4E: return TextureFormat::DXT5;
        case 0x4F: case 0x50: return TextureFormat::ATI1;
        case 0x52: case 0x53: return TextureFormat::ATI2;
        case 0x61: case 0x62: case 0x63: return TextureFormat::BC7;
        default: return TextureFormat::Unknown;
    }
}

namespace {
/// Bytes per block (block formats) or per pixel of a gen9 code, and the block edge; 0 for codes ragekit does not size.
uint32_t gen9_unit_bytes(uint8_t code, uint32_t& edge) {
    edge = 1;
    switch (code) {
        case 0x46: case 0x47: case 0x48: case 0x4F: case 0x50: case 0x51: edge = 4; return 8;
        case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x52: case 0x53: case 0x54:
        case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: edge = 4; return 16;
        case 0x02: return 16;
        case 0x0A: case 0x0B: return 8;
        case 0x18: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x29: case 0x57: case 0x5A: case 0x5B: return 4;
        case 0x36: case 0x38: case 0x55: case 0x56: case 0x73: return 2;
        case 0x3C: case 0x3D: case 0x41: return 1;
        default: return 0;
    }
}
} // namespace

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

// Enhanced: a 128 byte texture (rage::sga::Texture): the image parameters, the name, a shader resource view at 0x58 that
// 0x30 points at, the data and the usage word. CodeWalker's TextureBase/Texture (Gen9).
static Texture read_texture_gen9(const Reader& r, uint64_t ptr) {
    Texture t;
    TextureGen9 g;
    auto c = r.cursor(ptr + 8);
    const uint32_t block_count = c.u32();   // 0x08
    const uint32_t block_stride = c.u32();  // 0x0C
    g.flags = c.u32();                      // 0x10
    c.u32();                                // 0x14
    t.width = c.u16();                      // 0x18
    t.height = c.u16();
    t.depth = c.u16();
    g.dimension = c.u8();                   // 0x1E
    g.format = c.u8();                      // 0x1F
    g.tile_mode = c.u8();                   // 0x20
    c.u8();                                 // 0x21 anti-aliasing
    t.levels = c.u8();                      // 0x22
    g.unk23 = c.u8();                       // 0x23
    c.u8(); c.u8(); c.u16();                // 0x24 0x25, 0x26 usage count (1)
    const uint64_t name_ptr = c.u64();      // 0x28
    const uint64_t srv_ptr = c.u64();       // 0x30
    const uint64_t data_ptr = c.u64();      // 0x38
    t.usage_data = c.u32();                 // 0x40
    if (srv_ptr) g.srv_vft = r.u64(srv_ptr);
    t.name = r.str(name_ptr);
    t.format = texture_format_from_gen9(g.format);
    if (t.format != TextureFormat::Unknown && texture_format_gen9(t.format) == g.format) g.format = 0;   // implied by format
    t.stride = (uint16_t)texture_row_pitch(t.format, t.width);
    t.unk32 = 0x80;                         // what most of Legacy's dictionary textures carry
    if (data_ptr != 0) {
        size_t want = (size_t)block_count * block_stride;
        size_t have = r.remaining(data_ptr);
        if (want > have) want = have;
        t.data = r.bytes(data_ptr, want);
    }
    t.gen9 = g;
    return t;
}

Texture read_texture(const Reader& r, uint64_t ptr) {
    if (r.gen9()) return read_texture_gen9(r, ptr);
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
    Reader r(res, rsc::game_of("ytd", res.version));
    return read_txd(r, rsc::kSystemBase);
}

// ---------------------------------------------------------------- writing

// Virtual table pointers as they appear in game files. The game overwrites them on
// load, so any value works; these keep diffs against game files small.
static constexpr uint32_t kTextureVFT = 0x405A0B18;
static constexpr uint32_t kTxdVFT     = 0x40589E78;

// Enhanced: the layout read_texture_gen9 reads. The block count is the blocks (or pixels) of the whole mip chain.
static constexpr uint32_t kTextureVFTGen9 = 0x406B7940;
static constexpr uint32_t kTxdVFTGen9     = 0x4068C308;

static void gen9_image_params(BlockWriter& w, const Texture& t, const TextureGen9& g, uint8_t code) {
    uint32_t edge = 1;
    const uint32_t unit = gen9_unit_bytes(code, edge);
    uint64_t blocks = 0;
    uint32_t mw = t.width, mh = t.height;
    for (uint32_t i = 0; i < t.levels; i++) {
        blocks += (uint64_t)std::max(1u, (mw + edge - 1) / edge) * std::max(1u, (mh + edge - 1) / edge) * std::max<uint32_t>(1, t.depth);
        mw = std::max(1u, mw / 2);
        mh = std::max(1u, mh / 2);
    }
    if (unit == 0) blocks = 0;
    w.u32((uint32_t)blocks);                // 0x08
    w.u32(unit);                            // 0x0C
    w.u32(g.flags);                         // 0x10
    w.u32(0);
    w.u16(t.width);                         // 0x18
    w.u16(t.height);
    w.u16(t.depth);
    w.u8(g.dimension);                      // 0x1E
    w.u8(code);
    w.u8(g.tile_mode);
    w.u8(0);
    w.u8(t.levels);                         // 0x22
    w.u8(g.unk23);
    w.u8(0); w.u8(0);
    w.u16(1);                               // 0x26 usage count
}

// The usage word is m_PhysicalSizeAndTemplateType (texture.h). Legacy splits the template (type, flags, version) around the
// physical size; Enhanced keeps the template whole in bits 0-11 and has a field of its own in bits 19-23, which the game's
// Enhanced copies of Legacy textures set to 16, or 6 for most textures without mips.
static uint32_t usage_to_gen9(const Texture& t) {
    return t.template_type() | (t.levels > 1 ? 16u : 6u) << 19;
}
static uint32_t usage_to_legacy(const Texture& t) {
    const uint32_t tt = t.template_type();
    const uint32_t size = ((uint32_t)t.data.size() + 4095u) & ~4095u;   // what most of the game's textures record
    return (tt & 0x7F) | (size & 0x07FFFF80u) | ((tt >> 7) & 0x1F) << 27;
}

static BlockId write_texture_gen9(Builder& b, const Texture& t) {
    TextureGen9 g = t.gen9.value_or(TextureGen9{});
    if (!t.gen9 && t.unk32 != 0x80) g.unk23 = 0;   // what the game's Enhanced copies of Legacy textures hold
    const uint32_t usage = t.gen9 ? t.usage_data : usage_to_gen9(t);
    const uint8_t code = g.format ? g.format : texture_format_gen9(t.format);
    if (code == 0) throw Error("texture '" + t.name + "': no Enhanced format for " + texture_format_name(t.format));
    BlockId blk = b.alloc(Segment::System, 128);
    BlockId name = t.name.empty() ? BlockId{} : b.string(t.name);
    BlockId data = b.array(Segment::Graphics, t.data, 16);
    BlockWriter w(b, blk);
    w.u32(kTextureVFTGen9);
    w.u32(1);
    gen9_image_params(w, t, g, code);
    w.ptr(name);                            // 0x28
    w.ptr(blk, 0x58);                       // 0x30: the view below
    w.ptr(data);                            // 0x38
    w.u32(usage);                           // 0x40
    w.u32(2);
    w.u64(0);
    w.u64(0);                               // 0x50
    w.u64(g.srv_vft);                       // 0x58: shader resource view (Texture2D)
    w.u64(0);
    w.u16(t.depth > 1 ? 0x61 : 0x41);
    w.u16(0xFFFF);
    w.u32(0xFFFFFFFF);
    w.u64(0);
    w.u64(0);                               // 0x78
    return blk;
}

BlockId write_texture(Builder& b, const Texture& t) {
    if (b.gen9()) return write_texture_gen9(b, t);
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
    w.u32(t.gen9 ? usage_to_legacy(t) : t.usage_data);
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
    if (b.gen9()) {
        // what every stub of the game's Enhanced shaders holds: no image, one level, a 2D dimension, no view
        w.u32(kTextureVFTGen9);
        w.u32(1);
        w.u64(0);
        w.u32(0x00260000);
        w.u32(0);
        w.u16(0); w.u16(0); w.u16(1);   // 0x18 width, height, depth
        w.u8(1);                        // 0x1E 2D
        w.u8(0);
        w.u8(0xFF);                     // 0x20 tile mode
        w.u8(0);
        w.u8(1);                        // 0x22 levels
        w.u8(0); w.u8(0); w.u8(0);
        w.u16(1);                       // 0x26 usage count
        w.ptr(nameb);                   // 0x28
        w.zero(32);
        return blk;
    }
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
    const uint32_t txd_vft = b.gen9() ? kTxdVFTGen9 : kTxdVFT;
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
    w.u32(txd_vft);
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

Bytes write_ytd(const TextureDictionary& txd, Game game) {
    Builder b;
    b.set_game(game);
    write_txd(b, txd, nullptr, true);
    return b.build(rsc::version_for("ytd", game));
}

} // namespace rk
