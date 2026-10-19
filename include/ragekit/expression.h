// ragekit - expression dictionaries (.yed: the facial and secondary-motion expressions of peds) and frame filter
// dictionaries (.yfd: which bone tracks a blend weights how), the two other pgDictionaries of the animation system.
//
// Like clip dictionaries (clip.h) every structure is kept as read (graph.h) and written back the same; views read the
// fields tools need. Expressions run streams of compiled instructions (kept raw) over tracks, springs and variables.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "graph.h"
#include "types.h"

namespace rk {

enum class ExpressionKind : uint16_t {
    Dictionary = 1, PointerArray, Hashes, Expression, Stream, Tracks, Springs, String,
    FilterDictionary, Filter, FilterEntries, FilterWeights,
};

/// A track an expression drives: a bone (by tag), its track, and the value type in the low 7 bits of `flags` (0 vector3,
/// 1 quaternion, 2 float).
struct ExpressionTrack {
    uint16_t bone = 0;
    uint8_t track = 0;
    uint8_t flags = 0;
};
static_assert(sizeof(ExpressionTrack) == 4);

/// An expression (crExpressions, 0x90 bytes).
struct ExpressionRef {
    uint32_t hash = 0;      // its key in the dictionary
    RscBlockPtr block;

    std::string name() const { return block->string_at(0x60); }
    uint32_t signature() const { return block->get<uint32_t>(0x74); }
    size_t stream_count() const { return block->get<uint16_t>(0x28); }
    std::vector<ExpressionTrack> tracks() const;
    size_t spring_count() const { return block->get<uint16_t>(0x48); }
    std::vector<uint32_t> variables() const;
    /// The instruction streams: a 16-byte header (name hash, the three data lengths, depth) and the data.
    std::vector<RscBlockPtr> streams() const;
};

struct ExpressionDictionary {
    RscBlockPtr root;
    std::vector<ExpressionRef> expressions() const;
};

/// A frame filter (crFrameFilter): its type at 0x10 (4 multi-weight, the only one the game's files use), its signature.
struct FrameFilterRef {
    uint32_t hash = 0;
    RscBlockPtr block;

    uint32_t type() const { return block->get<uint32_t>(0x10); }
    uint32_t signature() const { return block->get<uint32_t>(0x0C); }
    /// Multi-weight: the (track, bone, weight index) entries and the weights.
    size_t entry_count() const { return type() == 4 ? block->get<uint16_t>(0x20) : 0; }
    std::vector<float> weights() const;
};

struct FrameFilterDictionary {
    RscBlockPtr root;
    std::vector<FrameFilterRef> filters() const;
};

/// .yed (version 25). Throws rk::Error for structures the schema does not know; `census` as read_ycd_checked.
ExpressionDictionary read_yed(std::span<const uint8_t> file, size_t* census = nullptr, std::string* where = nullptr);
Bytes write_yed(const ExpressionDictionary& d);
/// .yfd (version 4).
FrameFilterDictionary read_yfd(std::span<const uint8_t> file, size_t* census = nullptr, std::string* where = nullptr);
Bytes write_yfd(const FrameFilterDictionary& d);

}  // namespace rk
