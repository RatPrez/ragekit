#include "ragekit/particle.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include "ragekit/error.h"
#include "ragekit/hash.h"
#include "drawable_io.h"
#include "texture_io.h"

namespace rk {

std::string PtxBlock::string_at(uint32_t at) const {
    const Link* l = link(at);
    if (!l || !l->to || l->to->type != PtxType::String) return "";
    const Bytes& b = l->to->bytes;
    const size_t start = std::min<size_t>(l->offset, b.size());
    size_t end = start;
    while (end < b.size() && b[end]) end++;
    return std::string((const char*)b.data() + start, end - start);
}

std::string PtxRuleList::name(size_t i) const {
    if (i >= rules.size() || !rules[i]) return "";
    return rules[i]->string_at(0x20);
}

namespace {

// ---------------------------------------------------------------- the schema (CodeWalker's Particle.cs)

constexpr uint32_t kKfp = 0x90;   // ptxKeyframeProp, embedded in its owners; their keyframe prop lists point into them

/// Embedded keyframe props per behaviour type (after the 0x30 byte ptxBehaviour) and the behaviour's size.
struct BehaviourInfo { uint32_t type, size, kfps; };
constexpr BehaviourInfo kBehaviours[] = {
    {0xF5B33BAA, 0x30, 0},  {0xD63D9F1B, 0x170, 2}, {0x6C0719BC, 0x30, 0},  {0x1EE64552, 0x280, 4}, {0x38B60240, 0x280, 4},
    {0x052B1293, 0x170, 2}, {0x64E5D702, 0xD0, 1},  {0x928A1C45, 0x170, 2}, {0xECA84C1E, 0xD0, 1},  {0x164AEA72, 0x1F0, 3},
    {0x68FA73F5, 0x70, 0},  {0x38B63978, 0xF0, 1},  {0x0544C710, 0x550, 9}, {0x6232E25A, 0x40, 0},  {0x8F3B6036, 0x180, 2},
    {0xA35C721F, 0x170, 2}, {0xB77FED19, 0x280, 4}, {0x25AC9437, 0xC0, 1},  {0xC57377F8, 0xF0, 1},  {0xA05DA63E, 0x430, 7},
    {0xD4594BEF, 0x40, 0},  {0xA2D6DC3F, 0x50, 0},  {0xDF229542, 0x50, 0},
    {0x843768CF, 0xD0, 1},  // Enhanced only (ptfx_g9 effects)
};
const BehaviourInfo* behaviour_info(uint32_t type) {
    for (auto& b : kBehaviours) if (b.type == type) return &b;
    return nullptr;
}

uint32_t shader_var_size(uint8_t type) {
    switch (type) {
        case 2: case 4: case 6: return 0x40;   // vector2, vector4, texture
        case 7: return 0x50;                   // keyframe
        default: return 0;
    }
}

// ---------------------------------------------------------------- reading

class GraphReader {
public:
    GraphReader(const Reader& r, std::map<uint64_t, std::string> textures, std::map<uint64_t, uint32_t> drawables)
        : r_(r), textures_(std::move(textures)), drawables_(std::move(drawables)) {}

    std::shared_ptr<PtxBlock> read(uint64_t addr, PtxType type) {
        if (!addr) return nullptr;
        auto it = at_.find(addr);
        if (it != at_.end()) {
            if (it->second->type != type) throw Error("ypt: a " + std::to_string((int)type) + " shares its address with a " + std::to_string((int)it->second->type));
            return it->second;
        }
        size_t size = 0;
        switch (type) {
            case PtxType::RuleDictionary: case PtxType::EffectRuleDictionary: case PtxType::EmitterRuleDictionary: size = 0x40; break;
            case PtxType::ParticleRule: size = 0x240; break;
            case PtxType::EffectRule: size = 0x3C0; break;
            case PtxType::EmitterRule: size = 0x630; break;
            case PtxType::Domain: size = 0x280; break;
            case PtxType::EventEmitter: size = 0x70; break;
            case PtxType::EvolutionList: size = 0x40; break;
            case PtxType::Behaviour: {
                const BehaviourInfo* bi = behaviour_info(r_.u32(addr + 8));
                if (!bi) throw Error("ypt: behaviour type " + hash_name(r_.u32(addr + 8)) + " is not known");
                size = bi->size;
                break;
            }
            case PtxType::ShaderVar:
                size = shader_var_size(r_.u8(addr + 0x14));
                if (!size) throw Error("ypt: shader variable type " + std::to_string(r_.u8(addr + 0x14)) + " is not known");
                break;
            case PtxType::String: {
                const std::string s = r_.str(addr);
                size = s.size() + 1;
                break;
            }
            default: throw Error("ypt: a single structure of an array type");
        }
        return make(addr, type, size, [&](PtxBlock& b) { fields(b, addr, type, 0); });
    }

    /// An array of `count` elements of an array type (or raw bytes: element size 1).
    std::shared_ptr<PtxBlock> array(uint64_t addr, PtxType type, size_t count, size_t elem) {
        if (!addr || !count) return nullptr;
        auto it = at_.find(addr);
        if (it != at_.end()) return it->second;
        return make(addr, type, count * elem, [&](PtxBlock& b) {
            if (type == PtxType::Raw) return;
            for (size_t i = 0; i < count; i++) fields(b, addr + i * elem, type, (uint32_t)(i * elem));
        });
    }

    /// A Root block standing for the root structure, so pointers to it resolve.
    std::shared_ptr<PtxBlock> root(uint64_t addr, size_t size) {
        auto b = std::make_shared<PtxBlock>();
        b->type = PtxType::Root;
        b->bytes.resize(size);
        at_[addr] = b;
        return b;
    }

    /// The pointers of an owner that lead into blocks read elsewhere (keyframe prop lists, rules named by others).
    void resolve() {
        for (auto& p : weak_) {
            auto it = at_.upper_bound(p.target);
            if (it == at_.begin()) throw Error("ypt: a pointer leads before every structure");
            --it;
            const uint64_t start = it->first;
            if (p.target >= start + it->second->bytes.size()) throw Error("ypt: a pointer leads to no known structure (" + hash_name((uint32_t)p.target) + ")");
            p.from->links.push_back({p.at, it->second, (uint32_t)(p.target - start)});
        }
        weak_.clear();
        for (auto& [addr, b] : at_) std::sort(b->links.begin(), b->links.end(), [](const PtxBlock::Link& x, const PtxBlock::Link& y) { return x.at < y.at; });
    }

    /// Pointer-like values (in the system segment) that no schema field covers: the schema's blind spots.
    size_t census(std::string* first) const {
        size_t n = 0;
        for (auto& [addr, b] : at_) {
            if (b->type == PtxType::String || b->type == PtxType::Raw || b->type == PtxType::Root) continue;
            std::set<uint32_t> known;
            if (auto h = hashes_.find(b.get()); h != hashes_.end()) known = h->second;
            for (auto& l : b->links) known.insert(l.at);
            for (auto& t : b->textures) known.insert(t.at);
            for (auto& d : b->drawables) known.insert(d.at);
            for (uint32_t o = 0; o + 8 <= b->bytes.size(); o += 8) {
                uint64_t v;
                std::memcpy(&v, b->bytes.data() + o, 8);
                if (known.count(o)) continue;
                if ((v & 0xFFFFFFFFF0000000ull) == rsc::kSystemBase && (v - rsc::kSystemBase) < r_.resource().system.size() && v != rsc::kSystemBase) {
                    if (first) {
                        char v8[24]; std::snprintf(v8, sizeof v8, "=%llX", (unsigned long long)v);
                        *first += " type " + std::to_string((int)b->type) + (b->type == PtxType::Behaviour ? " " + hash_name(b->get<uint32_t>(8)) : "") + " +" + std::to_string(o) + v8;
                    }
                    n++;
                }
            }
        }
        return n;
    }

private:
    struct Weak { PtxBlock* from; uint32_t at; uint64_t target; };

    std::shared_ptr<PtxBlock> make(uint64_t addr, PtxType type, size_t size, const std::function<void(PtxBlock&)>& fill) {
        auto b = std::make_shared<PtxBlock>();
        b->type = type;
        b->bytes = r_.bytes(addr, size);
        at_[addr] = b;
        fill(*b);
        return b;
    }

    uint64_t take(PtxBlock& b, uint32_t at) {
        uint64_t v = 0;
        if (at + 8 > b.bytes.size()) throw Error("ypt: a pointer field past its structure");
        std::memcpy(&v, b.bytes.data() + at, 8);
        std::memset(b.bytes.data() + at, 0, 8);
        return v;
    }
    void own(PtxBlock& b, uint32_t at, PtxType t) {
        const uint64_t p = take(b, at);
        if (p) b.links.push_back({at, read(p, t), 0});
    }
    void weak(PtxBlock& b, uint32_t at) {
        const uint64_t p = take(b, at);
        if (p) weak_.push_back({&b, at, p});
    }
    void str(PtxBlock& b, uint32_t at) { own(b, at, PtxType::String); }
    /// ptr, u16 count, u16 capacity at `at`: an array of pointers to owned structures (or into others: `weak_targets`).
    void pointer_list(PtxBlock& b, uint64_t addr, uint32_t at, PtxType t, bool weak_targets, bool use_capacity) {
        const uint64_t arr = take(b, at);
        const uint16_t count = r_.u16(addr + at + 8), cap = r_.u16(addr + at + 10);
        const size_t n = use_capacity ? std::max<size_t>(count, cap) : count;
        if (!arr || !n) return;
        auto existing = at_.find(arr);
        std::shared_ptr<PtxBlock> a;
        if (existing != at_.end()) a = existing->second;
        else {
            a = make(arr, PtxType::PointerArray, n * 8, [&](PtxBlock& pa) {
                for (size_t i = 0; i < n; i++) {
                    if (weak_targets) weak(pa, (uint32_t)(i * 8));
                    else own(pa, (uint32_t)(i * 8), t);
                }
            });
        }
        b.links.push_back({at, a, 0});
    }
    /// ptr, u16 count, u16 capacity at `at`: an array of embedded structures.
    void list(PtxBlock& b, uint64_t addr, uint32_t at, PtxType t, uint32_t elem) {
        const uint64_t arr = take(b, at);
        const size_t n = r_.u16(addr + at + 8);
        if (arr && n) b.links.push_back({at, array(arr, t, n, elem), 0});
    }
    void kfp(PtxBlock& b, uint64_t addr, uint32_t base) {
        hashes_[&b].insert(base + 0x68);   // its name
        for (uint32_t k = 0; k < 12; k++) weak(b, base + 0x08 + 8 * k);   // the evolved keyframe prop, once per update thread
        list(b, addr, base + 0x70, PtxType::KeyframePropValues, 0x20);
    }

    /// Applies the schema of `type` to the structure at `addr`, which sits at `base` inside block `b`.
    void fields(PtxBlock& b, uint64_t addr, PtxType type, uint32_t base) {
        auto o = [&](uint32_t x) { return base + x; };
        auto at = [&](uint32_t x) { return addr + x; };   // absolute address of a field (for counts)
        switch (type) {
            case PtxType::RuleDictionary: case PtxType::EffectRuleDictionary: case PtxType::EmitterRuleDictionary: {
                const PtxType rule = type == PtxType::RuleDictionary ? PtxType::ParticleRule : type == PtxType::EffectRuleDictionary ? PtxType::EffectRule : PtxType::EmitterRule;
                list(b, addr - base, o(0x20), PtxType::Raw, 4);
                pointer_list(b, addr - base, o(0x30), rule, false, false);
                break;
            }
            case PtxType::ParticleRule:
                for (uint32_t s : {0x20u, 0x90u}) { weak(b, o(s + 0x58)); str(b, o(s + 0x60)); }   // effect spawners: the rule they spawn, its name
                str(b, o(0x120));
                for (uint32_t l = 0; l < 5; l++) pointer_list(b, addr - base, o(0x128 + 0x10 * l), PtxType::Behaviour, false, false);
                list(b, addr - base, o(0x188), PtxType::BiasLinks, 0x58);
                str(b, o(0x1B8));
                str(b, o(0x1C0));
                pointer_list(b, addr - base, o(0x1F0), PtxType::ShaderVar, false, false);
                list(b, addr - base, o(0x210), PtxType::ParticleDrawables, 0x30);
                break;
            case PtxType::EffectRule:
                str(b, o(0x20));
                weak(b, o(0x28));   // the effect list (the dictionary's root)
                pointer_list(b, addr - base, o(0x38), PtxType::EventEmitter, false, true);
                own(b, o(0x48), PtxType::EvolutionList);
                for (uint32_t k = 0; k < 5; k++) kfp(b, addr - base, o(0xC0 + k * kKfp));
                pointer_list(b, addr - base, o(0x390), PtxType::Raw, true, true);
                break;
            case PtxType::EmitterRule:
                str(b, o(0x20));
                for (uint32_t d : {0x38u, 0x48u, 0x58u}) own(b, o(d), PtxType::Domain);
                for (uint32_t k = 0; k < 10; k++) kfp(b, addr - base, o(0x78 + k * kKfp));
                {
                    // the list of the ten keyframe props above: count at 0x620, capacity at 0x622
                    const uint64_t arr = take(b, o(0x618));
                    const size_t n = std::max<size_t>(r_.u16(at(0x620)), r_.u16(at(0x622)));
                    if (arr && n) {
                        auto a = make(arr, PtxType::PointerArray, n * 8, [&](PtxBlock& pa) { for (size_t i = 0; i < n; i++) weak(pa, (uint32_t)(i * 8)); });
                        b.links.push_back({o(0x618), a, 0});
                    }
                }
                break;
            case PtxType::Behaviour: {
                const BehaviourInfo* bi = behaviour_info(r_.u32(at(8)));
                pointer_list(b, addr - base, o(0x10), PtxType::Raw, true, true);
                for (uint32_t k = 0; bi && k < bi->kfps; k++) kfp(b, addr - base, o(0x30 + k * kKfp));
                break;
            }
            case PtxType::Domain:
                for (uint32_t k = 0; k < 4; k++) kfp(b, addr - base, o(0x18 + k * kKfp));
                pointer_list(b, addr - base, o(0x260), PtxType::Raw, true, true);
                break;
            case PtxType::ShaderVar: {
                const uint8_t t = r_.u8(at(0x14));
                if (t == 6) {
                    const uint64_t tex = take(b, o(0x28));
                    if (tex) {
                        auto it = textures_.find(tex);
                        if (it == textures_.end()) throw Error("ypt: a shader variable's texture is not one of the dictionary's");
                        b.textures.push_back({o(0x28), it->second});
                    }
                    str(b, o(0x30));
                } else if (t == 7) {
                    list(b, addr - base, o(0x28), PtxType::ShaderVarKeyframeItems, 0x20);
                }
                break;
            }
            case PtxType::BiasLinks:
                list(b, addr - base, o(0x40), PtxType::Raw, 4);
                break;
            case PtxType::ParticleDrawables: {
                str(b, o(0x10));
                const uint64_t d = take(b, o(0x18));
                if (d) {
                    auto it = drawables_.find(d);
                    if (it == drawables_.end()) throw Error("ypt: a particle drawable is not one of the dictionary's");
                    b.drawables.push_back({o(0x18), it->second});
                }
                break;
            }
            case PtxType::EventEmitter:
                own(b, o(0x18), PtxType::EvolutionList);
                str(b, o(0x30));
                str(b, o(0x38));
                weak(b, o(0x40));
                weak(b, o(0x48));
                break;
            case PtxType::EvolutionList:
                list(b, addr - base, o(0x00), PtxType::Evolutions, 0x18);
                list(b, addr - base, o(0x10), PtxType::EvolvedKeyframeProps, 0x18);
                list(b, addr - base, o(0x28), PtxType::EvolvedKeyframePropMap, 0x10);
                break;
            case PtxType::Evolutions: str(b, o(0x00)); break;
            case PtxType::EvolvedKeyframePropMap:
                hashes_[&b].insert(o(0x00));   // the keyframe prop's name
                weak(b, o(0x08));
                break;
            case PtxType::EvolvedKeyframeProps:
                list(b, addr - base, o(0x00), PtxType::EvolvedKeyframes, 0x30);
                hashes_[&b].insert(o(0x10));   // its name
                break;
            case PtxType::EvolvedKeyframes: list(b, addr - base, o(0x00), PtxType::KeyframePropValues, 0x20); break;
            default: break;
        }
    }

    const Reader& r_;
    std::map<uint64_t, std::string> textures_;
    std::map<uint64_t, uint32_t> drawables_;
    std::map<uint64_t, std::shared_ptr<PtxBlock>> at_;
    std::vector<Weak> weak_;
    std::map<const PtxBlock*, std::set<uint32_t>> hashes_;   // offsets of name hashes (census: not pointers)
};

PtxRuleList read_rule_list(GraphReader& g, const Reader& r, uint64_t ptr, PtxType dict_type) {
    PtxRuleList l;
    if (!ptr) return l;
    l.dictionary = g.read(ptr, dict_type);
    const PtxBlock& d = *l.dictionary;
    if (const auto* h = d.link(0x20)) for (size_t i = 0; i + 4 <= h->to->bytes.size(); i += 4) l.hashes.push_back(h->to->get<uint32_t>(i));
    if (const auto* p = d.link(0x30))
        for (uint32_t i = 0; i < (uint32_t)p->to->bytes.size(); i += 8)
            if (const auto* x = p->to->link(i)) l.rules.push_back(x->to);
    (void)r;
    return l;
}

}   // namespace

std::vector<std::shared_ptr<PtxBlock>> ParticleDictionary::blocks() const {
    std::vector<std::shared_ptr<PtxBlock>> out;
    std::set<const PtxBlock*> seen;
    std::function<void(const std::shared_ptr<PtxBlock>&)> walk = [&](const std::shared_ptr<PtxBlock>& b) {
        if (!b || b->type == PtxType::Root || !seen.insert(b.get()).second) return;
        out.push_back(b);
        for (auto& l : b->links) walk(l.to);
    };
    for (auto* l : {&particle_rules, &effect_rules, &emitter_rules}) walk(l->dictionary);
    return out;
}

ParticleDictionary read_ypt_checked(std::span<const uint8_t> file, size_t* census, std::string* where) {
    rsc::Resource res = rsc::Resource::load(file);
    Reader r(res, rsc::game_of("ypt", res.version));
    ParticleDictionary p;
    const uint64_t root = rsc::kSystemBase;
    p.name = r.str(r.u64(root + 0x10));
    const uint64_t txd_ptr = r.u64(root + 0x20), dd_ptr = r.u64(root + 0x30);
    p.root_unk[0] = r.u64(root + 0x18); p.root_unk[1] = r.u64(root + 0x28); p.root_unk[2] = r.u64(root + 0x40); p.root_unk[3] = r.u64(root + 0x58);
    std::map<uint64_t, std::string> textures;
    if (txd_ptr) {
        p.textures = read_txd(r, txd_ptr);
        const auto ptrs = r.array<uint64_t>(r.u64(txd_ptr + 0x30), r.u16(txd_ptr + 0x38));
        for (auto t : ptrs) if (t) textures[t] = r.str(r.u64(t + 0x28));
    }
    std::map<uint64_t, uint32_t> drawables;
    if (dd_ptr) {
        p.drawable_hashes = r.array<uint32_t>(r.u64(dd_ptr + 0x20), r.u16(dd_ptr + 0x28));
        const auto ptrs = r.array<uint64_t>(r.u64(dd_ptr + 0x30), r.u16(dd_ptr + 0x38));
        for (size_t i = 0; i < ptrs.size(); i++) {
            Drawable d;
            if (ptrs[i]) { read_drawable_base(r, ptrs[i], d); drawables[ptrs[i]] = (uint32_t)i; }
            p.drawables.push_back(std::move(d));
        }
    }
    GraphReader g(r, std::move(textures), std::move(drawables));
    g.root(root, 0x60);
    p.particle_rules = read_rule_list(g, r, r.u64(root + 0x38), PtxType::RuleDictionary);
    p.effect_rules = read_rule_list(g, r, r.u64(root + 0x48), PtxType::EffectRuleDictionary);
    p.emitter_rules = read_rule_list(g, r, r.u64(root + 0x50), PtxType::EmitterRuleDictionary);
    g.resolve();
    if (census) *census = g.census(where);
    return p;
}

ParticleDictionary read_ypt(std::span<const uint8_t> file) { return read_ypt_checked(file, nullptr, nullptr); }

// ---------------------------------------------------------------- writing

namespace {
constexpr uint32_t kPtxListVft = 0x40605C78, kPtxDrawableDictVft = 0x405F9F40, kPtxDrawableVft = 0x40616BE8;
}

Bytes write_ypt(const ParticleDictionary& p, Game game) {
    if (p.drawable_hashes.size() != p.drawables.size()) throw Error("ypt: drawable_hashes and drawables differ in count");
    Builder b;
    b.set_game(game);
    const BlockId root = b.alloc(Segment::System, 0x60);
    b.pointer(root, 0x08, b.alloc_pages_info());
    {
        BlockWriter w(b, root);
        w.u32(kPtxListVft);
        w.u32(1);
        w.seek(0x18); w.u64(p.root_unk[0]);
        w.seek(0x28); w.u64(p.root_unk[1]);
        w.seek(0x40); w.u64(p.root_unk[2]);
        w.seek(0x58); w.u64(p.root_unk[3]);
    }
    if (!p.name.empty()) b.pointer(root, 0x10, b.string(p.name));
    TextureBlocks textures;
    if (p.textures) b.pointer(root, 0x20, write_txd(b, *p.textures, &textures, false));
    std::vector<BlockId> drawables;
    if (!p.drawables.empty() || !p.drawable_hashes.empty()) {
        const BlockId dict = b.alloc(Segment::System, 64);
        std::vector<const Drawable*> all;
        for (auto& d : p.drawables) all.push_back(&d);
        Gen9SamplerScope samplers(all);
        for (auto& d : p.drawables) {
            const BlockId blk = b.alloc(Segment::System, 176);
            const DrawableBlocks parts = prepare_drawable(b, d);
            BlockWriter w(b, blk);
            w.u32(kPtxDrawableVft);
            w.u32(1);
            w.u64(0);
            write_drawable_base(w, d, parts, b);
            drawables.push_back(blk);
        }
        const BlockId hashes = b.array(Segment::System, p.drawable_hashes);
        BlockId ptrs;
        if (!drawables.empty()) {
            ptrs = b.alloc(Segment::System, drawables.size() * 8);
            for (size_t i = 0; i < drawables.size(); i++) b.pointer(ptrs, i * 8, drawables[i]);
        }
        BlockWriter w(b, dict);
        w.u32(kPtxDrawableDictVft);
        w.u32(1);
        w.u64(0);
        w.u64(0);
        w.u64(1);
        w.ptr(hashes);
        w.u16((uint16_t)p.drawable_hashes.size());
        w.u16((uint16_t)p.drawable_hashes.size());
        w.u32(0);
        w.ptr(ptrs);
        w.u16((uint16_t)drawables.size());
        w.u16((uint16_t)drawables.size());
        w.u32(0);
        b.pointer(root, 0x30, dict);
    }
    // the rules: every block once, then their links
    std::map<const PtxBlock*, BlockId> ids;
    const auto all = p.blocks();
    for (auto& blk : all) {
        ids[blk.get()] = b.array(Segment::System, blk->bytes.data(), blk->bytes.size(), blk->type == PtxType::String ? 1 : 16);
    }
    for (auto& blk : all) {
        const BlockId from = ids.at(blk.get());
        for (auto& l : blk->links) {
            if (!l.to) continue;
            if (l.to->type == PtxType::Root) { b.pointer(from, l.at, root, l.offset); continue; }
            b.pointer(from, l.at, ids.at(l.to.get()), l.offset);
        }
        for (auto& t : blk->textures) {
            auto it = textures.find(joaat_lower(t.name));
            if (it == textures.end()) throw Error("ypt: a shader variable names texture '" + t.name + "', which the dictionary does not have");
            b.pointer(from, t.at, it->second);
        }
        for (auto& d : blk->drawables) {
            if (d.index >= drawables.size()) throw Error("ypt: a particle drawable names drawable " + std::to_string(d.index) + ", which the dictionary does not have");
            b.pointer(from, d.at, drawables[d.index]);
        }
    }
    if (p.particle_rules.dictionary) b.pointer(root, 0x38, ids.at(p.particle_rules.dictionary.get()));
    if (p.effect_rules.dictionary) b.pointer(root, 0x48, ids.at(p.effect_rules.dictionary.get()));
    if (p.emitter_rules.dictionary) b.pointer(root, 0x50, ids.at(p.emitter_rules.dictionary.get()));
    return b.build(rsc::version_for("ypt", game));
}

}   // namespace rk
