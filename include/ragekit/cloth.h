// ragekit - cloth: the character cloth of peds (.yld cloth dictionaries) and the environment cloth fragments carry
// (.yft): the simulated verlet meshes, the bridge between simulated and drawn vertices, the morph maps between LODs.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "types.h"

namespace rk {

struct Bound; // bounds.h

constexpr int kClothLods = 3;

/// A distance constraint between two simulated vertices (16 bytes).
struct ClothEdge {
    uint16_t vertex0 = 0, vertex1 = 0;
    float unk4 = 0, unk8 = 0, unkC = 0;
};

/// phVerletCloth: the simulated mesh of one LOD.
struct VerletCloth {
    std::vector<Vec4> vertices;
    std::vector<Vec4> vertices2;              // character cloth only, as many as vertices
    std::vector<ClothEdge> edges;
    std::vector<ClothEdge> custom_edges;      // rare
    std::shared_ptr<Bound> bound;             // environment cloth: a composite of capsules and planes
    Vec3 bbox_min, bbox_max;
    uint32_t unk3C = 0, unk4C = 0;            // 0x7f7fffff / 0xff7fffff in fragments
    float unkA8 = 9999, unkAC = 9999;
    uint32_t unkE8 = 0;
    uint16_t unkFA = 0, unk148 = 0;
    float unk158 = 1;
};

/// clothBridgeSimGfx: per LOD, what ties the simulated vertices to the drawn ones.
struct ClothBridge {
    std::array<uint32_t, kClothLods> vertex_counts{};
    std::array<std::vector<float>, kClothLods> unk20, unk60, unkA0;
    std::array<std::vector<uint16_t>, kClothLods> vertex_maps;
    std::vector<uint32_t> vertex_bits;        // one bit per vertex
    uint16_t bit_count = 0;
};

struct ClothMorphMap {
    std::vector<Vec4> unk50, unkA0;
    std::array<std::vector<uint16_t>, 4> unk60, unkB0;
    std::vector<uint16_t> unk150, unk160;
    uint32_t unk180 = 0;
};

/// phMorphController: maps between the LODs of an environment cloth.
struct ClothMorph {
    std::array<std::optional<ClothMorphMap>, kClothLods> maps;
};

struct ClothController {
    std::string name;                         // up to 32 characters
    uint32_t type = 3;                        // 2 character, 3 environment
    float unk78 = 0;
    std::optional<ClothBridge> bridge;
    std::optional<ClothMorph> morph;
    std::array<std::optional<VerletCloth>, kClothLods> verlet;
};

struct ClothBoneWeights {
    Vec4 weights;
    uint32_t bones[4] = {0, 0, 0, 0};         // into CharacterClothController::bone_ids
};

struct CharacterClothController : ClothController {
    std::vector<uint16_t> indices;            // triangles
    std::vector<Vec4> vertices;
    std::vector<ClothBoneWeights> bone_weights;
    std::vector<uint32_t> bone_ids;
    std::vector<uint32_t> unkB0;              // as many as bone_ids
    float unkA0 = 0.04f, unkDC = 1;
    CharacterClothController() { type = 2; unk78 = 1; }
};

struct CharacterCloth {
    std::optional<CharacterClothController> controller;
    std::shared_ptr<Bound> bound;             // a composite of capsules
    std::vector<uint32_t> bone_ids;           // the bone of each bound child
    Mat4 transform;
    std::vector<Vec4> unk10;
    std::vector<uint32_t> unk90;
};

struct ClothTuning {
    float unk10 = 3.14159274f, unk14 = 0.5235988f;
    float unk20 = 0, unk24 = 0, unk28 = 0, unk2C = 0;
    uint32_t flags = 0;
    float unk34 = -1, unk38 = 0;
    uint32_t unk3C = 0;
};

/// A fragment's cloth. It draws through the fragment's drawable_cloth when the fragment has one, else its drawable.
struct EnvironmentCloth {
    std::optional<ClothController> controller;
    std::optional<ClothTuning> tuning;
    std::vector<uint32_t> unk60;
    uint32_t unk78 = 0;                       // 0, or 16 / 48 with a tuning
};

struct ClothDictionary {
    std::vector<uint32_t> hashes;             // parallel to cloths; joaat of the name
    std::vector<CharacterCloth> cloths;
    const CharacterCloth* find(uint32_t hash) const;
};

ClothDictionary read_yld(std::span<const uint8_t> file);
Bytes write_yld(const ClothDictionary& d);

} // namespace rk
