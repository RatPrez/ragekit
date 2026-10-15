// ragekit - the game's string hashes.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace rk {

/// Jenkins one-at-a-time hash ("joaat") over the bytes of `s` as given.
/// Most game names are hashed lowercased: use joaat_lower for those.
uint32_t joaat(std::string_view s);

/// joaat of the ASCII-lowercased string (what the game does for most names).
uint32_t joaat_lower(std::string_view s);

/// Bone tags: ELF hash of the uppercased name, folded into the 16-bit tag range.
uint16_t bone_tag(std::string_view bone_name);

/// "hash_12AB34CD" style name for a hash with no known string.
std::string hash_name(uint32_t h);

/// Parses "hash_XXXXXXXX" / "0xXXXXXXXX" / decimal, else joaat_lower(s).
uint32_t hash_or_name(std::string_view s);

} // namespace rk
