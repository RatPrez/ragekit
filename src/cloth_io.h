// ragekit internal - cloth structures inside a resource, shared with fragments.
#pragma once
#include "ragekit/cloth.h"
#include "ragekit/rsc.h"

namespace rk {

/// Reads a 208 byte characterCloth and everything under it.
CharacterCloth read_character_cloth(const Reader& r, uint64_t ptr);
/// Reads a 128 byte environmentCloth; `drawable` receives its drawable pointer (a FragDrawable of the fragment).
EnvironmentCloth read_environment_cloth(const Reader& r, uint64_t ptr, uint64_t* drawable = nullptr);

BlockId write_character_cloth(Builder& b, const CharacterCloth& c);
/// `drawable`: the block of the fragment drawable the cloth draws through (the fragment writer's).
BlockId write_environment_cloth(Builder& b, const EnvironmentCloth& c, BlockId drawable);

} // namespace rk
