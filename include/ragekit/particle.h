// ragekit - particle effect dictionaries (.ypt): the textures and drawables particles draw with, and the rules that make
// effects (ptxEffectRule), emitters (ptxEmitterRule) and particles (ptxParticleRule).
//
// The texture dictionary and the drawables are the ordinary structs (texture.h, drawable.h), so they are read from and
// written for either edition like .ytd and .ydr files. The rules are a deep graph of some forty structures (behaviours,
// domains, keyframe properties, evolutions, shader variables, event emitters ...) that point at each other and into each
// other; ragekit keeps them as PtxBlocks: each structure's bytes with its pointers turned into links, read and laid out
// by a schema of every structure (CodeWalker's Particle.cs). The few values a tool needs are exposed; the rest stays as read.
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "drawable.h"
#include "game.h"
#include "texture.h"

namespace rk {

/// The structure types of a particle dictionary's rules.
enum class PtxType : uint16_t {
    Raw = 0,            // plain data: name hash arrays, keyframe values ...
    Root,               // stands for the dictionary's root structure (effect rules point at it)
    String,             // a NUL terminated string
    PointerArray,       // an array of pointers (its links)
    RuleDictionary, EffectRuleDictionary, EmitterRuleDictionary,
    ParticleRule, EffectRule, EmitterRule,
    Behaviour, Domain, ShaderVar,
    KeyframePropValues, BiasLinks, ParticleDrawables, EventEmitter, EvolutionList,
    Evolutions, EvolvedKeyframeProps, EvolvedKeyframePropMap, EvolvedKeyframes, ShaderVarKeyframeItems,
};

/// One structure (or array of structures) of the rules: its bytes with the pointers cleared, and where they point.
struct PtxBlock {
    PtxType type = PtxType::Raw;
    Bytes bytes;
    struct Link { uint32_t at = 0; std::shared_ptr<PtxBlock> to; uint32_t offset = 0; };   // a pointer at `at` to `to` + offset
    std::vector<Link> links;
    struct TextureLink { uint32_t at = 0; std::string name; };    // a pointer to a texture of the dictionary's own
    std::vector<TextureLink> textures;
    struct DrawableLink { uint32_t at = 0; uint32_t index = 0; }; // a pointer to a drawable of the dictionary's own
    std::vector<DrawableLink> drawables;

    template <class T> T get(size_t at) const { T v{}; if (at + sizeof(T) <= bytes.size()) std::memcpy(&v, bytes.data() + at, sizeof(T)); return v; }
    template <class T> void set(size_t at, T v) { if (at + sizeof(T) <= bytes.size()) std::memcpy(bytes.data() + at, &v, sizeof(T)); }
    const Link* link(uint32_t at) const { for (auto& l : links) if (l.at == at) return &l; return nullptr; }
    /// The string a link at `at` leads to, "" for none.
    std::string string_at(uint32_t at) const;
};

/// One of the three rule dictionaries: the rules in the file's order with the name hashes the game keys them by.
struct PtxRuleList {
    std::vector<uint32_t> hashes;
    std::vector<std::shared_ptr<PtxBlock>> rules;
    std::shared_ptr<PtxBlock> dictionary;   // the dictionary structure itself (64 bytes), kept for its other fields
    /// The rule's name (each rule carries it as a string).
    std::string name(size_t i) const;
};

struct ParticleDictionary {
    std::string name;
    std::optional<TextureDictionary> textures;
    std::vector<uint32_t> drawable_hashes;  // parallel to drawables
    std::vector<Drawable> drawables;        // the models particles draw (a drawable without name, lights or bound)
    PtxRuleList particle_rules, effect_rules, emitter_rules;
    uint64_t root_unk[4] = {0, 0, 0, 0};    // 0x18 0x28 0x40 0x58 of the root as read (always 0 in the game's files)

    /// Every block of the rules, each once (in the order write_ypt lays them out).
    std::vector<std::shared_ptr<PtxBlock>> blocks() const;
};

/// .ypt of either edition (version 68 Legacy, 71 Enhanced: only the textures and drawables differ).
ParticleDictionary read_ypt(std::span<const uint8_t> file);
/// read_ypt, and how many 8-byte values of the rules look like pointers (into the system segment) at places the schema
/// knows no pointer or name hash: 0 on every file of both editions; a check of the schema against new files.
ParticleDictionary read_ypt_checked(std::span<const uint8_t> file, size_t* census, std::string* where = nullptr);
Bytes write_ypt(const ParticleDictionary& p, Game game = default_game());

} // namespace rk
