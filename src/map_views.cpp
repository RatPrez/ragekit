// Typed views of the MetaObjects a MapData keeps: instanced grass, LOD lights, any field by name.
#include <algorithm>
#include <cstring>
#include <optional>
#include "ragekit/error.h"
#include "ragekit/hash.h"
#include "ragekit/map.h"

namespace rk {

namespace {

// The element types of the arrays (the structure names' hashes the game's files use)
constexpr uint32_t kGrassList = 0x7C475B5D;       // rage__fwGrassInstanceListDef
constexpr uint32_t kGrassInstance = 0xED86F522;   // rage__fwGrassInstanceListDef__InstanceData
constexpr uint32_t kFloatXYZ = 0xE2CBCFD4;        // VECTOR3 items (12 bytes)

template <class T> T get(const Bytes& b, size_t at) { T v{}; if (at + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + at, sizeof(T)); return v; }
template <class T> void put(Bytes& b, size_t at, T v) { if (at + sizeof(T) <= b.size()) std::memcpy(b.data() + at, &v, sizeof(T)); }
Vec3 vec3(const Bytes& b, size_t at) { return {get<float>(b, at), get<float>(b, at + 4), get<float>(b, at + 8)}; }
void put3(Bytes& b, size_t at, const Vec3& v) { put(b, at, v.x); put(b, at + 4, v.y); put(b, at + 8, v.z); }

const MetaLink* link_at(const MetaObject& o, uint32_t offset) {
    for (const MetaLink& l : o.links) if (l.offset == offset) return &l;
    return nullptr;
}

/// Replaces the array link at `offset`; without one, removes it and clears the array header (the writer keeps a header's
/// capacity margin, so a replaced array keeps its header).
void set_link(MetaObject& o, uint32_t offset, std::optional<MetaLink> l) {
    o.links.erase(std::remove_if(o.links.begin(), o.links.end(), [&](const MetaLink& x) { return x.offset == offset; }), o.links.end());
    if (!l) {
        if (offset + 16 <= o.data.size()) std::memset(o.data.data() + offset, 0, 16);
        return;
    }
    l->offset = offset;
    if (l->kind == MetaLink::Kind::Structs || l->kind == MetaLink::Kind::Pointers) l->count = (uint32_t)l->items.size();
    o.links.push_back(std::move(*l));
    std::sort(o.links.begin(), o.links.end(), [](const MetaLink& a, const MetaLink& b) { return a.offset < b.offset; });
}

template <class T> std::vector<T> primitives(const MetaObject& o, uint32_t offset) {
    std::vector<T> v;
    const MetaLink* l = link_at(o, offset);
    if (!l || l->kind != MetaLink::Kind::Primitives) return v;
    v.resize(l->raw.size() / sizeof(T));
    if (!v.empty()) std::memcpy(v.data(), l->raw.data(), v.size() * sizeof(T));
    return v;
}

template <class T> void set_primitives(MetaObject& o, uint32_t offset, uint32_t block_type, const std::vector<T>& v) {
    if (v.empty()) { set_link(o, offset, std::nullopt); return; }
    const MetaLink* old = link_at(o, offset);
    MetaLink l;
    l.kind = MetaLink::Kind::Primitives;
    l.block_type = old ? old->block_type : block_type;
    l.count = (uint32_t)v.size();
    l.raw.resize(v.size() * sizeof(T));
    std::memcpy(l.raw.data(), v.data(), l.raw.size());
    set_link(o, offset, std::move(l));
}

std::vector<Vec3> vectors(const MetaObject& o, uint32_t offset) {
    std::vector<Vec3> v;
    if (const MetaLink* l = link_at(o, offset)) for (const MetaObject& it : l->items) v.push_back(vec3(it.data, 0));
    return v;
}

void set_vectors(MetaObject& o, uint32_t offset, const std::vector<Vec3>& v) {
    if (v.empty()) { set_link(o, offset, std::nullopt); return; }
    const MetaLink* old = link_at(o, offset);
    MetaLink l;
    l.kind = MetaLink::Kind::Structs;
    l.block_type = old ? old->block_type : kFloatXYZ;
    for (size_t i = 0; i < v.size(); i++) {
        MetaObject it;
        if (old && i < old->items.size()) it = old->items[i];   // keeps what the 12 bytes do not hold
        else { it.type = l.block_type; it.data.assign(12, 0); }
        put3(it.data, 0, v[i]);
        l.items.push_back(std::move(it));
    }
    set_link(o, offset, std::move(l));
}

}  // namespace

Vec3 GrassBatch::position(const GrassInstance& g) const {
    const Vec3 s = box_max - box_min;
    return {box_min.x + s.x * g.position[0] / 65535.f, box_min.y + s.y * g.position[1] / 65535.f, box_min.z + s.z * g.position[2] / 65535.f};
}

std::vector<GrassBatch> grass_batches(const MapData& d) {
    std::vector<GrassBatch> out;
    const MetaLink* list = link_at(d.instanced_data, 32);
    if (!list) return out;
    for (const MetaObject& b : list->items) {
        GrassBatch g;
        g.box_min = vec3(b.data, 0);
        g.box_max = vec3(b.data, 16);
        g.scale_range = vec3(b.data, 32);
        g.archetype = get<uint32_t>(b.data, 48);
        g.lod_dist = get<uint32_t>(b.data, 52);
        g.lod_fade_start_dist = get<float>(b.data, 56);
        g.lod_inst_fade_range = get<float>(b.data, 60);
        g.orient_to_terrain = get<float>(b.data, 64);
        if (const MetaLink* inst = link_at(b, 72))
            for (const MetaObject& i : inst->items) {
                GrassInstance gi;
                std::memcpy(&gi, i.data.data(), std::min<size_t>(16, i.data.size()));
                g.instances.push_back(gi);
            }
        out.push_back(std::move(g));
    }
    return out;
}

void set_grass_batches(MapData& d, const std::vector<GrassBatch>& batches) {
    MetaObject& root = d.instanced_data;
    if (root.data.size() < 48) {
        if (batches.empty()) return;
        root.type = joaat_lower("rage__fwinstancedmapdata");
        root.data.assign(48, 0);
    }
    if (batches.empty()) { set_link(root, 32, std::nullopt); return; }
    const MetaLink* old = link_at(root, 32);
    MetaLink l;
    l.kind = MetaLink::Kind::Structs;
    l.block_type = old ? old->block_type : kGrassList;
    for (size_t k = 0; k < batches.size(); k++) {
        const GrassBatch& g = batches[k];
        MetaObject b;
        if (old && k < old->items.size()) b = old->items[k];
        else { b.type = l.block_type; b.data.assign(96, 0); }
        put3(b.data, 0, g.box_min);
        put3(b.data, 16, g.box_max);
        put3(b.data, 32, g.scale_range);
        put(b.data, 48, g.archetype);
        put(b.data, 52, g.lod_dist);
        put(b.data, 56, g.lod_fade_start_dist);
        put(b.data, 60, g.lod_inst_fade_range);
        put(b.data, 64, g.orient_to_terrain);
        const MetaLink* oi = link_at(b, 72);
        if (g.instances.empty()) set_link(b, 72, std::nullopt);
        else {
            MetaLink il;
            il.kind = MetaLink::Kind::Structs;
            il.block_type = oi ? oi->block_type : kGrassInstance;
            il.items.reserve(g.instances.size());
            for (const GrassInstance& gi : g.instances) {
                MetaObject i;
                i.type = il.block_type;
                i.data.resize(16);
                std::memcpy(i.data.data(), &gi, 16);
                il.items.push_back(std::move(i));
            }
            set_link(b, 72, std::move(il));
        }
        l.items.push_back(std::move(b));
    }
    set_link(root, 32, std::move(l));
}

LodLights lod_lights(const MapData& d) {
    LodLights out;
    const MetaObject& a = d.lod_lights;
    const MetaObject& b = d.distant_lod_lights;
    const std::vector<Vec3> dir = vectors(a, 8), pos = vectors(b, 8);
    const auto fall = primitives<float>(a, 24), fexp = primitives<float>(a, 40);
    const auto flags = primitives<uint32_t>(a, 56), hash = primitives<uint32_t>(a, 72), rgbi = primitives<uint32_t>(b, 24);
    const auto ci = primitives<uint8_t>(a, 88), co = primitives<uint8_t>(a, 104), cor = primitives<uint8_t>(a, 120);
    const size_t n = std::max(pos.size(), dir.size());
    for (size_t i = 0; i < n; i++) {
        LodLight l;
        if (i < pos.size()) l.position = pos[i];
        if (i < rgbi.size()) l.rgbi = rgbi[i];
        if (i < dir.size()) l.direction = dir[i];
        if (i < fall.size()) l.falloff = fall[i];
        if (i < fexp.size()) l.falloff_exponent = fexp[i];
        if (i < flags.size()) l.time_and_state_flags = flags[i];
        if (i < hash.size()) l.hash = hash[i];
        if (i < ci.size()) l.cone_inner = ci[i];
        if (i < co.size()) l.cone_outer_or_cap_ext = co[i];
        if (i < cor.size()) l.corona_intensity = cor[i];
        out.lights.push_back(l);
    }
    out.num_street_lights = get<uint16_t>(b.data, 40);
    out.category = get<uint16_t>(b.data, 42);
    out.has_lod = !a.links.empty();
    out.has_distant = !b.links.empty();
    return out;
}

void set_lod_lights(MapData& d, const LodLights& l) {
    MetaObject& a = d.lod_lights;
    MetaObject& b = d.distant_lod_lights;
    if (a.data.size() < 136 || b.data.size() < 48) throw Error("ymap: no LOD light objects to write the lights into");
    std::vector<Vec3> dir, pos;
    std::vector<float> fall, fexp;
    std::vector<uint32_t> flags, hash, rgbi;
    std::vector<uint8_t> ci, co, cor;
    for (const LodLight& x : l.lights) {
        pos.push_back(x.position); rgbi.push_back(x.rgbi); dir.push_back(x.direction); fall.push_back(x.falloff); fexp.push_back(x.falloff_exponent);
        flags.push_back(x.time_and_state_flags); hash.push_back(x.hash); ci.push_back(x.cone_inner); co.push_back(x.cone_outer_or_cap_ext); cor.push_back(x.corona_intensity);
    }
    if (l.has_lod) {
        set_vectors(a, 8, dir);
        set_primitives(a, 24, metatype::kFloat, fall);
        set_primitives(a, 40, metatype::kFloat, fexp);
        set_primitives(a, 56, metatype::kUInt, flags);
        set_primitives(a, 72, metatype::kUInt, hash);
        set_primitives(a, 88, metatype::kByte, ci);
        set_primitives(a, 104, metatype::kByte, co);
        set_primitives(a, 120, metatype::kByte, cor);
    }
    if (l.has_distant) {
        set_vectors(b, 8, pos);
        set_primitives(b, 24, metatype::kUInt, rgbi);
    }
    put(b.data, 40, l.num_street_lights);
    put(b.data, 42, l.category);
}

MetaField meta_field(const MetaObject& o, std::string_view name) {
    MetaField f;
    const MetaStruct* s = known_struct(o.type);
    if (!s) return f;
    const uint32_t h = joaat(name), hl = joaat_lower(name);   // field names hash as written ("offsetPosition"); a few lower case
    for (size_t i = 0; i < s->entries.size(); i++) {
        const MetaEntry& e = s->entries[i];
        if (e.name != h && e.name != hl) continue;
        size_t size = 0;
        switch (e.type) {
            case metatype::Boolean: case metatype::SignedByte: case metatype::UnsignedByte: case metatype::ByteEnum: size = 1; break;
            case metatype::SignedShort: case metatype::UnsignedShort: case metatype::ShortFlags: size = 2; break;
            case metatype::Float_XYZ: size = 12; break;
            case metatype::Float_XYZW: size = 16; break;
            case metatype::Array: case metatype::CharPointer: size = 16; break;
            case metatype::DataBlockPointer: case metatype::StructurePointer: size = 8; break;
            case metatype::ArrayOfBytes: case metatype::ArrayOfChars: size = e.ref_key; break;
            default: size = 4;
        }
        if (e.offset < 0 || (size_t)e.offset + size > o.data.size()) return f;
        f.data = o.data.data() + e.offset;
        f.type = e.type;
        f.size = size;
        return f;
    }
    return f;
}

}  // namespace rk
