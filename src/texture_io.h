// ragekit internal - texture structures inside a resource.
#pragma once
#include <map>
#include "ragekit/texture.h"
#include "ragekit/rsc.h"

namespace rk {

/// Reads a full Texture (144 byte struct + data) at ptr.
Texture read_texture(const Reader& r, uint64_t ptr);
/// Reads the 80 byte TextureBase stub a shader parameter points at: just the name.
std::string read_texture_ref(const Reader& r, uint64_t ptr);
/// Reads a TextureDictionary struct (64 bytes) at ptr.
TextureDictionary read_txd(const Reader& r, uint64_t ptr);

/// Written texture blocks by name hash, so shader parameters can point at embedded textures.
using TextureBlocks = std::map<uint32_t, BlockId>;

BlockId write_texture(Builder& b, const Texture& t);
BlockId write_texture_ref(Builder& b, const std::string& name);
/// Writes the dictionary struct and everything under it; `root` true makes it the
/// resource root (pages info allocated right after it).
BlockId write_txd(Builder& b, const TextureDictionary& txd, TextureBlocks* blocks, bool root);

} // namespace rk
