// ragekit - textures and texture dictionaries (.ytd, and the ones embedded in drawables).
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "game.h"
#include "types.h"

namespace rk {

/// The game's D3D9-style format codes (what the Texture struct stores).
enum class TextureFormat : uint32_t {
    Unknown  = 0,
    A8R8G8B8 = 21,
    X8R8G8B8 = 22,
    A1R5G5B5 = 25,
    A8       = 28,
    A8B8G8R8 = 32,
    L8       = 50,
    DXT1 = 0x31545844, // BC1
    DXT3 = 0x33545844, // BC2
    DXT5 = 0x35545844, // BC3
    ATI1 = 0x31495441, // BC4
    ATI2 = 0x32495441, // BC5
    BC7  = 0x20374342,
};

const char* texture_format_name(TextureFormat f);
TextureFormat texture_format_from_name(std::string_view name);
bool texture_format_is_block(TextureFormat f);
/// Bytes per 4x4 block for block formats, bytes per pixel otherwise.
uint32_t texture_format_unit_bytes(TextureFormat f);
/// Size in bytes of one mip level.
size_t texture_level_size(TextureFormat f, uint32_t w, uint32_t h);
/// Row pitch (bytes per row of blocks or pixels) of the top level.
uint32_t texture_row_pitch(TextureFormat f, uint32_t w);
/// Size in bytes of a full mip chain.
size_t texture_chain_size(TextureFormat f, uint32_t w, uint32_t h, uint32_t levels, uint32_t depth = 1);

/// Enhanced (gen9) textures store rage::sga::BufferFormat codes, in DXGI_FORMAT order (BC1 0x47, BC3 0x4D, BC7 0x62 ...).
/// The code of a format (0 for Unknown).
uint8_t texture_format_gen9(TextureFormat f);
/// The format of a gen9 code. The sRGB codes give the format with the same bytes (BC1_UNORM_SRGB -> DXT1); codes Legacy
/// has no format for give Unknown.
TextureFormat texture_format_from_gen9(uint8_t code);

/// Texture "usage" values the game stores in the low 5 bits of the usage word.
enum class TextureUsage : uint8_t {
    Unknown = 0, Default = 1, Terrain = 2, CloudDensity = 3, CloudNormal = 4, Cable = 5, Fence = 6,
    EnvEff = 7, Script = 8, WaterFlow = 9, WaterFoam = 10, WaterFog = 11, WaterOcean = 12, Water = 13,
    FoamOpacity = 14, Foam = 15, DiffuseMipSharpen = 16, DiffuseDetail = 17, DiffuseDark = 18,
    DiffuseAlphaOpaque = 19, Diffuse = 20, Detail = 21, Normal = 22, Specular = 23, Emissive = 24,
    TintPalette = 25, SkipProcessing = 26, DoNotOptimize = 27, Test = 28, Count = 29
};

/// What an Enhanced texture stores beyond the fields Texture shares with Legacy. Read from Enhanced files and written back
/// as it was; an Enhanced write of a texture without it writes these defaults (what most of the game's textures carry).
struct TextureGen9 {
    uint8_t format = 0;          // the gen9 format code (texture_format_gen9); 0 = derived from Texture::format
    uint8_t dimension = 1;       // 1 2D, 2 cube, 3 3D
    uint8_t tile_mode = 0xFF;    // 0xFF: chosen by the driver (every texture of the game)
    uint8_t unk23 = 0x28;        // 0x28 or 0 (half of the game's textures each)
    uint32_t flags = 0x00260208; // 0x00260228 on script render targets
    uint64_t srv_vft = 0x1406B77D8;  // the view's vtable (any value works; the game overwrites it)
};

struct Texture {
    std::string name;
    uint16_t width = 0, height = 0, depth = 1;
    uint16_t stride = 0;     // row pitch of level 0; recomputed on write if 0
    TextureFormat format = TextureFormat::DXT1;
    uint8_t levels = 1;
    uint32_t usage_data = 0; // usage (low 5 bits) | usage flags << 5
    uint32_t extra_flags = 0;
    uint16_t unk32 = 0x20;   // observed 0x20..0x90 on dictionary textures, 2 on shader-parameter stubs (Legacy only)
    Bytes data;              // the full mip chain, level 0 first (the same bytes in both editions)
    std::optional<TextureGen9> gen9;   // set by reading an Enhanced file

    TextureUsage usage() const { return (TextureUsage)(usage_data & 0x1F); }
    void set_usage(TextureUsage u) { usage_data = (usage_data & ~0x1Fu) | ((uint32_t)u & 0x1F); }
    uint32_t usage_flags() const { return usage_data >> 5; }
    uint32_t name_hash() const;
    /// Byte range of one mip level inside data.
    std::span<const uint8_t> level(uint32_t level, uint32_t& w, uint32_t& h) const;
};

struct TextureDictionary {
    std::vector<Texture> textures; // kept sorted by name hash, as the game stores them

    const Texture* find(uint32_t name_hash) const;
    const Texture* find(std::string_view name) const;
    void add(Texture t);           // inserts keeping hash order, replacing a same-hash entry
    void sort();
};

/// .ytd files of either edition (the version tells them apart).
TextureDictionary read_ytd(std::span<const uint8_t> file);
/// A .ytd for an edition (game.h): version 13 for Legacy, 5 for Enhanced.
Bytes write_ytd(const TextureDictionary& txd, Game game = default_game());

} // namespace rk
