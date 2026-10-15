#include "ragekit/cloth.h"
#include "ragekit/bounds.h"
#include "ragekit/error.h"
#include "cloth_io.h"
#include "bounds_io.h"
#include <algorithm>
#include <cstring>

namespace rk {

static_assert(sizeof(ClothEdge) == 16);
static_assert(sizeof(ClothBoneWeights) == 32);

const CharacterCloth* ClothDictionary::find(uint32_t h) const {
    for (size_t i = 0; i < hashes.size() && i < cloths.size(); i++)
        if (hashes[i] == h) return &cloths[i];
    return nullptr;
}

namespace {

namespace vft {
constexpr uint32_t kDictionary      = 1079456968u;
constexpr uint32_t kCharacterCloth  = 1080047184u;
constexpr uint32_t kCharController  = 1080055120u;
constexpr uint32_t kEnvironmentCloth = 1080059352u;
constexpr uint32_t kEnvController   = 1080089368u;
constexpr uint32_t kMorphController = 1080058840u;
constexpr uint32_t kTuning          = 1080058880u;
constexpr uint32_t kBridge          = 1080095072u;
constexpr uint32_t kVerletCloth     = 1080216392u;
constexpr uint32_t kVerletCloth50   = 1080219576u;
}

// ================================================================ reading

template <class T> std::vector<T> list(const Reader& r, uint64_t at) { return r.array<T>(r.u64(at), r.u16(at + 8)); }

std::string fixed_string(const Reader& r, uint64_t at, size_t n) {
    const char* p = (const char*)r.resource().at(at, n);
    return std::string(p, std::find(p, p + n, '\0'));
}

VerletCloth read_verlet(const Reader& r, uint64_t p) {
    VerletCloth v;
    v.bound = read_bound(r, r.u64(p + 0x18));
    auto c = r.cursor(p + 0x30);
    v.bbox_min = c.vec3(); v.unk3C = c.u32();
    v.bbox_max = c.vec3(); v.unk4C = c.u32();
    v.vertices2 = list<Vec4>(r, p + 0x70);
    v.vertices = list<Vec4>(r, p + 0x80);
    v.unkA8 = r.f32(p + 0xA8);
    v.unkAC = r.f32(p + 0xAC);
    v.unkE8 = r.u32(p + 0xE8);
    v.unkFA = r.u16(p + 0xFA);
    v.custom_edges = list<ClothEdge>(r, p + 0x100);
    v.edges = list<ClothEdge>(r, p + 0x110);
    v.unk148 = r.u16(p + 0x148);
    v.unk158 = r.f32(p + 0x158);
    return v;
}

ClothBridge read_bridge(const Reader& r, uint64_t p) {
    ClothBridge g;
    for (int l = 0; l < kClothLods; l++) {
        g.vertex_counts[l] = r.u32(p + 0x10 + 4 * l);
        g.unk20[l] = list<float>(r, p + 0x20 + 16 * l);
        g.unk60[l] = list<float>(r, p + 0x60 + 16 * l);
        g.unkA0[l] = list<float>(r, p + 0xA0 + 16 * l);
        g.vertex_maps[l] = list<uint16_t>(r, p + 0xE0 + 16 * l);
    }
    g.vertex_bits = list<uint32_t>(r, p + 0x128);
    g.bit_count = r.u16(p + 0x132);
    return g;
}

ClothMorphMap read_morph_map(const Reader& r, uint64_t p) {
    ClothMorphMap m;
    m.unk50 = list<Vec4>(r, p + 0x50);
    m.unkA0 = list<Vec4>(r, p + 0xA0);
    for (int i = 0; i < 4; i++) {
        m.unk60[i] = list<uint16_t>(r, p + 0x60 + 16 * i);
        m.unkB0[i] = list<uint16_t>(r, p + 0xB0 + 16 * i);
    }
    m.unk150 = list<uint16_t>(r, p + 0x150);
    m.unk160 = list<uint16_t>(r, p + 0x160);
    m.unk180 = r.u32(p + 0x180);
    return m;
}

void read_controller(const Reader& r, uint64_t p, ClothController& c) {
    uint64_t bridge = r.u64(p + 0x10), morph = r.u64(p + 0x18);
    if (bridge) c.bridge = read_bridge(r, bridge);
    if (morph) {
        c.morph.emplace();
        for (int l = 0; l < kClothLods; l++)
            if (uint64_t mp = r.u64(morph + 0x18 + 8 * l)) c.morph->maps[l] = read_morph_map(r, mp);
    }
    for (int l = 0; l < kClothLods; l++)
        if (uint64_t vp = r.u64(p + 0x20 + 8 * l)) c.verlet[l] = read_verlet(r, vp);
    c.type = r.u32(p + 0x50);
    c.name = fixed_string(r, p + 0x58, 32);
    c.unk78 = r.f32(p + 0x78);
}

CharacterClothController read_character_controller(const Reader& r, uint64_t p) {
    CharacterClothController c;
    read_controller(r, p, c);
    c.indices = list<uint16_t>(r, p + 0x80);
    c.vertices = list<Vec4>(r, p + 0x90);
    c.unkA0 = r.f32(p + 0xA0);
    c.unkB0 = list<uint32_t>(r, p + 0xB0);
    c.bone_weights = list<ClothBoneWeights>(r, p + 0xC0);
    c.unkDC = r.f32(p + 0xDC);
    c.bone_ids = list<uint32_t>(r, p + 0xE0);
    return c;
}

ClothTuning read_tuning(const Reader& r, uint64_t p) {
    ClothTuning t;
    t.unk10 = r.f32(p + 0x10); t.unk14 = r.f32(p + 0x14);
    auto c = r.cursor(p + 0x20);
    t.unk20 = c.f32(); t.unk24 = c.f32(); t.unk28 = c.f32(); t.unk2C = c.f32();
    t.flags = c.u32();
    t.unk34 = c.f32(); t.unk38 = c.f32();
    t.unk3C = c.u32();
    return t;
}

} // namespace

CharacterCloth read_character_cloth(const Reader& r, uint64_t p) {
    CharacterCloth c;
    c.unk10 = list<Vec4>(r, p + 0x10);
    if (uint64_t cp = r.u64(p + 0x20)) c.controller = read_character_controller(r, cp);
    c.bound = read_bound(r, r.u64(p + 0x28));
    c.bone_ids = list<uint32_t>(r, p + 0x30);
    c.transform = r.at<Mat4>(p + 0x50);
    c.unk90 = list<uint32_t>(r, p + 0x90);
    return c;
}

EnvironmentCloth read_environment_cloth(const Reader& r, uint64_t p, uint64_t* drawable) {
    EnvironmentCloth e;
    if (uint64_t tp = r.u64(p + 0x10)) e.tuning = read_tuning(r, tp);
    if (drawable) *drawable = r.u64(p + 0x18);
    if (uint64_t cp = r.u64(p + 0x28)) {
        e.controller.emplace();
        read_controller(r, cp, *e.controller);
    }
    e.unk60 = r.array<uint32_t>(r.u64(p + 0x60), r.u16(p + 0x68));
    e.unk78 = r.u32(p + 0x78);
    return e;
}

ClothDictionary read_yld(std::span<const uint8_t> file) {
    rsc::Resource res = rsc::Resource::load(file);
    Reader r(res);
    ClothDictionary d;
    d.hashes = list<uint32_t>(r, rsc::kSystemBase + 0x20);
    for (uint64_t p : list<uint64_t>(r, rsc::kSystemBase + 0x30)) d.cloths.push_back(p ? read_character_cloth(r, p) : CharacterCloth{});
    return d;
}

// ================================================================ writing

namespace {

uint16_t count16(size_t n, const char* what) {
    if (n > 0xFFFF) throw Error(std::string("cloth: too many ") + what);
    return (uint16_t)n;
}

template <class T> void write_list(Builder& b, BlockWriter& w, size_t at, const std::vector<T>& v, const char* what, int capacity = -1) {
    uint16_t n = count16(v.size(), what);
    w.seek(at);
    w.ptr(v.empty() ? BlockId{} : b.array(Segment::System, v));
    w.u16(n);
    w.u16(capacity < 0 ? n : (uint16_t)capacity);
    w.u32(0);
}

BlockId write_verlet(Builder& b, const VerletCloth& v) {
    BlockId blk = b.alloc(Segment::System, 0x180);
    BlockId bound = v.bound ? write_bound(b, *v.bound) : BlockId{};
    BlockId behavior = b.alloc(Segment::System, 0x40);
    BlockId unk140 = b.alloc(Segment::System, 0x10);
    BlockWriter w(b, blk);
    w.u32(vft::kVerletCloth);
    w.u32(1);
    w.seek(0x18);
    w.ptr(bound);
    w.seek(0x30);
    w.vec3(v.bbox_min); w.u32(v.unk3C);
    w.vec3(v.bbox_max); w.u32(v.unk4C);
    w.u32(vft::kVerletCloth50);
    w.u32(1);
    write_list(b, w, 0x70, v.vertices2, "verlet vertices");
    write_list(b, w, 0x80, v.vertices, "verlet vertices");
    w.seek(0xA8);
    w.f32(v.unkA8); w.f32(v.unkAC);
    w.seek(0xE8);
    w.u32(v.unkE8);
    w.u32((uint32_t)v.edges.size());
    w.u32((uint32_t)v.vertices.size());
    w.u32(0);
    w.u16(3);
    w.u16(v.unkFA);
    write_list(b, w, 0x100, v.custom_edges, "verlet edges");
    write_list(b, w, 0x110, v.edges, "verlet edges");
    w.seek(0x130);
    w.ptr(behavior);
    w.u64(0x100000);
    w.ptr(unk140);
    w.u16(v.unk148);
    w.u16(count16(v.vertices.size(), "verlet vertices"));
    w.seek(0x158);
    w.f32(v.unk158);
    return blk;
}

BlockId write_bridge(Builder& b, const ClothBridge& g) {
    BlockId blk = b.alloc(Segment::System, 0x140);
    BlockWriter w(b, blk);
    w.u32(vft::kBridge);
    w.u32(1);
    for (int l = 0; l < kClothLods; l++) {
        w.seek(0x10 + 4 * l);
        w.u32(g.vertex_counts[l]);
        write_list(b, w, 0x20 + 16 * l, g.unk20[l], "bridge values");
        write_list(b, w, 0x60 + 16 * l, g.unk60[l], "bridge values");
        write_list(b, w, 0xA0 + 16 * l, g.unkA0[l], "bridge values");
        write_list(b, w, 0xE0 + 16 * l, g.vertex_maps[l], "bridge map entries");
    }
    write_list(b, w, 0x128, g.vertex_bits, "bridge bit words", g.bit_count);
    return blk;
}

BlockId write_morph_map(Builder& b, const ClothMorphMap& m) {
    BlockId blk = b.alloc(Segment::System, 0x190);
    BlockWriter w(b, blk);
    write_list(b, w, 0x50, m.unk50, "morph values");
    write_list(b, w, 0xA0, m.unkA0, "morph values");
    for (int i = 0; i < 4; i++) {
        write_list(b, w, 0x60 + 16 * i, m.unk60[i], "morph values");
        write_list(b, w, 0xB0 + 16 * i, m.unkB0[i], "morph values");
    }
    write_list(b, w, 0x150, m.unk150, "morph values");
    write_list(b, w, 0x160, m.unk160, "morph values");
    w.seek(0x180);
    w.u32(m.unk180);
    return blk;
}

BlockId write_morph(Builder& b, const ClothMorph& m) {
    BlockId blk = b.alloc(Segment::System, 0x40);
    BlockWriter w(b, blk);
    w.u32(vft::kMorphController);
    w.u32(1);
    w.seek(0x18);
    for (auto& map : m.maps) w.ptr(map ? write_morph_map(b, *map) : BlockId{});
    return blk;
}

void write_controller(Builder& b, BlockWriter& w, const ClothController& c, uint32_t vft) {
    if (c.name.size() > 32) throw Error("cloth: controller name longer than 32 characters");
    w.seek(0);
    w.u32(vft);
    w.u32(1);
    w.seek(0x10);
    w.ptr(c.bridge ? write_bridge(b, *c.bridge) : BlockId{});
    w.ptr(c.morph ? write_morph(b, *c.morph) : BlockId{});
    for (auto& v : c.verlet) w.ptr(v ? write_verlet(b, *v) : BlockId{});
    w.seek(0x50);
    w.u32(c.type);
    w.seek(0x58);
    w.bytes(c.name.data(), c.name.size());
    w.seek(0x78);
    w.f32(c.unk78);
}

BlockId write_character_controller(Builder& b, const CharacterClothController& c) {
    BlockId blk = b.alloc(Segment::System, 0xF0);
    BlockWriter w(b, blk);
    write_controller(b, w, c, vft::kCharController);
    write_list(b, w, 0x80, c.indices, "cloth indices");
    write_list(b, w, 0x90, c.vertices, "cloth vertices");
    w.seek(0xA0);
    w.f32(c.unkA0);
    write_list(b, w, 0xB0, c.unkB0, "cloth bone values");
    write_list(b, w, 0xC0, c.bone_weights, "cloth bone weights");
    w.seek(0xDC);
    w.f32(c.unkDC);
    write_list(b, w, 0xE0, c.bone_ids, "cloth bones");
    return blk;
}

BlockId write_tuning(Builder& b, const ClothTuning& t) {
    BlockId blk = b.alloc(Segment::System, 0x40);
    BlockWriter w(b, blk);
    w.u32(vft::kTuning);
    w.u32(1);
    w.seek(0x10);
    w.f32(t.unk10); w.f32(t.unk14);
    w.seek(0x20);
    w.f32(t.unk20); w.f32(t.unk24); w.f32(t.unk28); w.f32(t.unk2C);
    w.u32(t.flags);
    w.f32(t.unk34); w.f32(t.unk38);
    w.u32(t.unk3C);
    return blk;
}

} // namespace

BlockId write_character_cloth(Builder& b, const CharacterCloth& c) {
    BlockId blk = b.alloc(Segment::System, 0xD0);
    BlockWriter w(b, blk);
    w.u32(vft::kCharacterCloth);
    w.u32(1);
    write_list(b, w, 0x10, c.unk10, "cloth values");
    w.seek(0x20);
    w.ptr(c.controller ? write_character_controller(b, *c.controller) : BlockId{});
    w.ptr(c.bound ? write_bound(b, *c.bound) : BlockId{});
    write_list(b, w, 0x30, c.bone_ids, "cloth bones");
    w.seek(0x50);
    w.bytes(c.transform.m.data(), 64);
    write_list(b, w, 0x90, c.unk90, "cloth values");
    w.seek(0xC0);
    w.u64(1);
    return blk;
}

BlockId write_environment_cloth(Builder& b, const EnvironmentCloth& e, BlockId drawable) {
    BlockId blk = b.alloc(Segment::System, 0x80);
    BlockWriter w(b, blk);
    w.u32(vft::kEnvironmentCloth);
    w.u32(1);
    w.seek(0x10);
    w.ptr(e.tuning ? write_tuning(b, *e.tuning) : BlockId{});
    w.ptr(drawable);
    w.seek(0x28);
    if (e.controller) {
        BlockId cb = b.alloc(Segment::System, 0x80);
        BlockWriter cw(b, cb);
        write_controller(b, cw, *e.controller, vft::kEnvController);
        w.ptr(cb);
    }
    uint16_t n = count16(e.unk60.size(), "cloth values");
    w.seek(0x60);
    w.ptr(e.unk60.empty() ? BlockId{} : b.array(Segment::System, e.unk60));
    w.u16(n);
    w.u16(n);
    w.seek(0x78);
    w.u32(e.unk78);
    return blk;
}

Bytes write_yld(const ClothDictionary& d) {
    if (d.hashes.size() != d.cloths.size()) throw Error("cloth dictionary: hashes and cloths differ in count");
    std::vector<size_t> order(d.cloths.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t c) { return d.hashes[a] < d.hashes[c]; });

    Builder b;
    BlockId root = b.alloc(Segment::System, 0x40);
    BlockId pages = b.alloc_pages_info();
    std::vector<uint32_t> hashes;
    for (size_t i : order) hashes.push_back(d.hashes[i]);
    BlockId ptrs = d.cloths.empty() ? BlockId{} : b.alloc(Segment::System, d.cloths.size() * 8);
    for (size_t i = 0; i < order.size(); i++) b.pointer(ptrs, i * 8, write_character_cloth(b, d.cloths[order[i]]));
    BlockWriter w(b, root);
    w.u32(vft::kDictionary);
    w.u32(1);
    w.ptr(pages);
    w.u64(0);
    w.u64(1);
    write_list(b, w, 0x20, hashes, "cloths");
    uint16_t n = count16(d.cloths.size(), "cloths");
    w.seek(0x30);
    w.ptr(ptrs);
    w.u16(n);
    w.u16(n);
    return b.build(rsc::kYld);
}

} // namespace rk
