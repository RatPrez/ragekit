// ragekit - plain RGBA images: decode game textures to pixels, encode pixels to
// game formats, and DDS / PNG files for interchange.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "texture.h"

namespace rk {

/// 8-bit RGBA, row-major, top-left first.
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    uint8_t* px(uint32_t x, uint32_t y) { return &rgba[(size_t)(y * width + x) * 4]; }
    const uint8_t* px(uint32_t x, uint32_t y) const { return &rgba[(size_t)(y * width + x) * 4]; }
};

/// Decodes one mip level of a texture to RGBA. Supports every TextureFormat.
Image decode_texture(const Texture& t, uint32_t level = 0);
/// Decodes raw texel data of the given format.
Image decode_texels(TextureFormat f, uint32_t w, uint32_t h, std::span<const uint8_t> data);

/// Encodes RGBA into a texture of the given format with a full mip chain
/// (levels = 0 means down to 1x1). DXT1/DXT5/ATI1/ATI2/A8R8G8B8/A8B8G8R8/A8/L8 supported.
Texture encode_texture(const Image& img, TextureFormat f, const std::string& name, uint32_t levels = 0);

/// Box-filtered half-size image (for mip chains).
Image downsample(const Image& img);

/// DDS files: the standard container for the game's texel data.
Texture read_dds(std::span<const uint8_t> file, const std::string& name);
Bytes write_dds(const Texture& t);

/// PNG (8-bit RGB/RGBA/grey, non-interlaced) via zlib.
Image read_png(std::span<const uint8_t> file);
Bytes write_png(const Image& img);

} // namespace rk
