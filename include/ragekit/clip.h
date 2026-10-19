// ragekit - clip dictionaries (.ycd): the game's animations and the clips that play them.
//
// A dictionary holds two hash maps: animations (crAnimation: frames, duration, the bone tracks and their compressed
// sequences) and clips (crClip: a name, the animation or list of animations it plays with start / end times and rate, and
// its properties and tags). ragekit keeps every structure as read (graph.h), so a dictionary is written back exactly as
// it was; the views below read the fields tools need and change the simple ones in place.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "graph.h"
#include "types.h"

namespace rk {

/// The structures of a clip dictionary (RscBlock::kind).
enum class ClipKind : uint16_t {
    Dictionary = 1, AnimationMap, AnimationEntry, Animation, Sequence, BoneIds,
    ClipEntry, Clip, ClipAnimations, PropertyMap, PropertyEntry, Property, Attribute, TagList, Tag,
    PointerArray, String,
};

enum class ClipType : uint32_t { Animation = 1, AnimationList = 2 };

/// A track an animation drives: a bone (by its tag) and what of it (track 0 translation, 1 rotation, 2 scale ...).
struct AnimationBoneId {
    uint16_t bone = 0;
    uint8_t format = 0;    // the track's value type: 0 vector3, 1 quaternion, 2 float (each track number has one)
    uint8_t track = 0;
};
static_assert(sizeof(AnimationBoneId) == 4);

/// An animation (crAnimation, 0x60 bytes) of the dictionary.
struct AnimationRef {
    uint32_t hash = 0;     // its key in the dictionary
    RscBlockPtr block;

    uint16_t frames() const { return block->get<uint16_t>(0x14); }
    uint16_t sequence_frame_limit() const { return block->get<uint16_t>(0x16); }
    float duration() const { return block->get<float>(0x18); }
    void set_duration(float d) { block->set(0x18, d); }
    /// 0x10: a sequence carries root motion (set exactly then); 0x01 and 0x08 seen on a few animations without.
    uint8_t flags() const { return block->get<uint8_t>(0x10); }
    uint32_t max_sequence_block_length() const { return block->get<uint32_t>(0x38); }
    /// 1 + the number of clips of the dictionary that play it (on every game animation).
    uint32_t usage_count() const { return block->get<uint32_t>(0x3C); }
    std::vector<AnimationBoneId> bone_ids() const;
    /// The sequences (blocks of the compressed channel data: a 32-byte header and the data), in order.
    std::vector<RscBlockPtr> sequences() const;
};

/// A clip (crClip, 0x70 bytes): what plays.
struct ClipRef {
    uint32_t hash = 0;     // its key in the dictionary (joaat of the lowercased name without its path)
    RscBlockPtr block;

    std::string name() const { return block->string_at(0x18); }
    ClipType type() const { return (ClipType)block->get<uint32_t>(0x10); }
    /// Animation clips: the animation's start and end in seconds and the rate; a list's duration.
    float start_time() const { return type() == ClipType::Animation ? block->get<float>(0x58) : 0.f; }
    float end_time() const { return type() == ClipType::Animation ? block->get<float>(0x5C) : block->get<float>(0x60); }
    float rate() const { return type() == ClipType::Animation ? block->get<float>(0x60) : 1.f; }
    /// The animations it plays (one for an animation clip, the list's for a list).
    std::vector<RscBlockPtr> animations() const;
    /// The name hashes of its properties.
    std::vector<uint32_t> property_names() const;
    size_t tag_count() const;
};

struct ClipDictionary {
    RscBlockPtr root;      // the crClipDictionary, everything else reachable from it

    std::vector<AnimationRef> animations() const;
    std::vector<ClipRef> clips() const;
    /// The animation stored under a key, an empty ref (block null) when there is none.
    AnimationRef animation(uint32_t hash) const;
    ClipRef clip(uint32_t hash) const;
};

/// .ycd of either edition (version 46). Throws rk::Error for structures the schema does not know.
ClipDictionary read_ycd(std::span<const uint8_t> file);
/// read_ycd, and how many 8-byte values of the structures look like pointers where the schema takes none (0 on every
/// game file: a check of the schema).
ClipDictionary read_ycd_checked(std::span<const uint8_t> file, size_t* census, std::string* where = nullptr);
Bytes write_ycd(const ClipDictionary& d);

}  // namespace rk
