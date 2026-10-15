#include "ragekit/map.h"
#include "ragekit/hash.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rk {

namespace {

// Structure name hashes (joaat of the type names; verified against files by the tests).
struct Names {
    uint32_t CMapTypes = joaat("CMapTypes");
    uint32_t CMapData = joaat("CMapData");
    uint32_t CBaseArchetypeDef = joaat("CBaseArchetypeDef");
    uint32_t CTimeArchetypeDef = joaat("CTimeArchetypeDef");
    uint32_t CMloArchetypeDef = joaat("CMloArchetypeDef");
    uint32_t CMloRoomDef = joaat("CMloRoomDef");
    uint32_t CMloPortalDef = joaat("CMloPortalDef");
    uint32_t CMloEntitySet = joaat("CMloEntitySet");
    uint32_t CMloTimeCycleModifier = joaat("CMloTimeCycleModifier");
    uint32_t CEntityDef = joaat("CEntityDef");
    uint32_t CMloInstanceDef = joaat("CMloInstanceDef");
    uint32_t CTimeCycleModifier = joaat("CTimeCycleModifier");
    uint32_t CCarGen = joaat("CCarGen");
    uint32_t BoxOccluder = joaat("BoxOccluder");
    uint32_t OccludeModel = joaat("OccludeModel");
    uint32_t rage__fwContainerLodDef = joaat("rage__fwContainerLodDef");
    uint32_t rage__fwInstancedMapData = joaat("rage__fwInstancedMapData");
    uint32_t CLODLight = joaat("CLODLight");
    uint32_t CDistantLODLight = joaat("CDistantLODLight");
    uint32_t CCompositeEntityType = joaat("CCompositeEntityType");
};
const Names& N() { static const Names n; return n; }

// Struct sizes (the layouts of CodeWalker's MetaTypes.cs).
constexpr size_t kMapTypes = 80, kBaseArch = 144, kTimeArch = 160, kMloArch = 240, kRoom = 112, kPortal = 64,
                 kEntitySet = 48, kMloTcm = 48, kEntity = 128, kMloInstance = 160, kMapData = 512, kTcm = 64,
                 kCarGen = 80, kBoxOccluder = 16;

[[noreturn]] void fail(const std::string& s) { throw Error("map: " + s); }

struct In {
    const uint8_t* p;
    template <class T> T get(size_t o) const { T v; std::memcpy(&v, p + o, sizeof v); return v; }
    uint32_t u32(size_t o) const { return get<uint32_t>(o); }
    int32_t  i32(size_t o) const { return get<int32_t>(o); }
    float    f32(size_t o) const { return get<float>(o); }
    Vec3 v3(size_t o) const { return {f32(o), f32(o + 4), f32(o + 8)}; }
    Vec4 v4(size_t o) const { return {f32(o), f32(o + 4), f32(o + 8), f32(o + 12)}; }
    MetaArray arr(size_t o) const { return Meta::read_array(p + o); }
};

struct Out {
    Bytes b;
    explicit Out(size_t n, const Bytes& raw = {}) : b(n, 0) { std::memcpy(b.data(), raw.data(), std::min(n, raw.size())); }
    template <class T> void put(size_t o, T v) { std::memcpy(b.data() + o, &v, sizeof v); }
    void u32(size_t o, uint32_t v) { put(o, v); }
    void i32(size_t o, int32_t v) { put(o, v); }
    void f32(size_t o, float v) { put(o, v); }
    void v3(size_t o, const Vec3& v) { f32(o, v.x); f32(o + 4, v.y); f32(o + 8, v.z); }
    void v4(size_t o, const Vec4& v) { f32(o, v.x); f32(o + 4, v.y); f32(o + 8, v.z); f32(o + 12, v.w); }
    void arr(size_t o, const MetaPtr& p) {
        MetaArray a = p.array();
        std::memcpy(b.data() + o, &a.pointer, 8);
        std::memcpy(b.data() + o + 8, &a.count, 2);
        std::memcpy(b.data() + o + 10, &a.capacity, 2);
    }
    void bytes(size_t o, const Bytes& v) { std::memcpy(b.data() + o, v.data(), v.size()); }
};

std::string read_string(const Meta& m, const MetaArray& a) {
    if (!a.pointer) return {};
    if (a.count) {
        const uint8_t* p = m.at(a.pointer, a.count);
        return std::string((const char*)p, a.count);
    }
    return m.string_at(a.pointer);
}

std::vector<uint32_t> read_uints(const Meta& m, const MetaArray& a) {
    std::vector<uint32_t> v(a.count);
    if (a.count && a.pointer) std::memcpy(v.data(), m.at(a.pointer, 4ull * a.count), 4ull * a.count);
    else v.clear();
    return v;
}

std::vector<MetaObject> read_pointer_objects(const Meta& m, const MetaArray& a) {
    std::vector<MetaObject> out;
    if (!a.pointer || !a.count) return out;
    const uint8_t* p = m.at(a.pointer, 8ull * a.count);
    for (uint32_t i = 0; i < a.count; i++) {
        uint64_t ptr; std::memcpy(&ptr, p + 8ull * i, 8);
        out.push_back(detach_object(m, ptr));
    }
    return out;
}

std::vector<MetaObject> read_struct_objects(const Meta& m, const MetaArray& a, uint32_t type) {
    std::vector<MetaObject> out;
    if (!a.pointer || !a.count) return out;
    const MetaStruct* info = m.find_struct(type);
    if (!info) info = known_struct(type);
    if (!info) fail("no description for structure " + hash_name(type));
    const uint8_t* p = m.at(a.pointer, (size_t)info->size * a.count);
    for (uint32_t i = 0; i < a.count; i++)
        out.push_back(detach_object(m, type, p + (size_t)i * info->size, (size_t)info->size * (a.count - i)));
    return out;
}

// --- entity -------------------------------------------------------------------------------------

Entity read_entity(const Meta& m, uint64_t ptr) {
    const MetaBlock& blk = m.block(meta_block_id(ptr));
    bool mlo = blk.type == N().CMloInstanceDef;
    if (blk.type != N().CEntityDef && !mlo) fail("entity of unsupported type " + hash_name(blk.type));
    In r{m.at(ptr, mlo ? kMloInstance : kEntity)};
    Entity e;
    e.archetype_name = r.u32(8);
    e.flags = r.u32(12);
    e.guid = r.u32(16);
    e.position = r.v3(32);
    e.rotation = r.v4(48);
    e.scale_xy = r.f32(64);
    e.scale_z = r.f32(68);
    e.parent_index = r.i32(72);
    e.lod_dist = r.f32(76);
    e.child_lod_dist = r.f32(80);
    e.lod_level = r.i32(84);
    e.num_children = r.u32(88);
    e.priority_level = r.i32(92);
    e.extensions = read_pointer_objects(m, r.arr(96));
    e.ambient_occlusion_multiplier = r.i32(112);
    e.artificial_ambient_occlusion = r.i32(116);
    e.tint_value = r.u32(120);
    e.raw.assign(r.p, r.p + (mlo ? kMloInstance : kEntity));
    if (mlo) {
        e.is_mlo_instance = true;
        e.group_id = r.u32(128);
        e.floor_id = r.u32(132);
        e.default_entity_sets = read_uints(m, r.arr(136));
        e.num_exit_portals = r.u32(152);
        e.mlo_inst_flags = r.u32(156);
    }
    return e;
}

std::vector<Entity> read_entities(const Meta& m, const MetaArray& a) {
    std::vector<Entity> out;
    if (!a.pointer || !a.count) return out;
    const uint8_t* p = m.at(a.pointer, 8ull * a.count);
    for (uint32_t i = 0; i < a.count; i++) {
        uint64_t ptr; std::memcpy(&ptr, p + 8ull * i, 8);
        out.push_back(read_entity(m, ptr));
    }
    return out;
}

MetaPtr write_entity(MetaBuilder& mb, const Entity& e) {
    Out o(e.is_mlo_instance ? kMloInstance : kEntity, e.raw);
    o.u32(8, e.archetype_name);
    o.u32(12, e.flags);
    o.u32(16, e.guid);
    o.v3(32, e.position);
    o.v4(48, e.rotation);
    o.f32(64, e.scale_xy);
    o.f32(68, e.scale_z);
    o.i32(72, e.parent_index);
    o.f32(76, e.lod_dist);
    o.f32(80, e.child_lod_dist);
    o.i32(84, e.lod_level);
    o.u32(88, e.num_children);
    o.i32(92, e.priority_level);
    o.arr(96, mb.add_object_pointers(e.extensions));
    o.i32(112, e.ambient_occlusion_multiplier);
    o.i32(116, e.artificial_ambient_occlusion);
    o.u32(120, e.tint_value);
    if (e.is_mlo_instance) {
        o.u32(128, e.group_id);
        o.u32(132, e.floor_id);
        o.arr(136, mb.add_uint_array(e.default_entity_sets, metatype::kHash));
        o.u32(152, e.num_exit_portals);
        o.u32(156, e.mlo_inst_flags);
    }
    return mb.add_item(e.is_mlo_instance ? N().CMloInstanceDef : N().CEntityDef, o.b);
}

MetaPtr write_entities(MetaBuilder& mb, const std::vector<Entity>& v) {
    if (v.empty()) return {};
    std::vector<uint64_t> ptrs;
    for (auto& e : v) ptrs.push_back(write_entity(mb, e).pointer());
    return mb.add_pointer_array(ptrs);
}

// --- interiors ----------------------------------------------------------------------------------

MloRoom read_room(const Meta& m, In r) {
    MloRoom x;
    x.name = read_string(m, r.arr(8));
    x.bb_min = r.v3(32);
    x.bb_max = r.v3(48);
    x.blend = r.f32(64);
    x.timecycle_name = r.u32(68);
    x.secondary_timecycle_name = r.u32(72);
    x.flags = r.u32(76);
    x.portal_count = r.u32(80);
    x.floor_id = r.i32(84);
    x.exterior_visibility_depth = r.i32(88);
    x.attached_objects = read_uints(m, r.arr(96));
    x.raw.assign(r.p, r.p + kRoom);
    return x;
}
Bytes write_room(MetaBuilder& mb, const MloRoom& x) {
    Out o(kRoom, x.raw);
    o.arr(8, x.name.empty() ? MetaPtr{} : mb.add_string(x.name));
    o.v3(32, x.bb_min);
    o.v3(48, x.bb_max);
    o.f32(64, x.blend);
    o.u32(68, x.timecycle_name);
    o.u32(72, x.secondary_timecycle_name);
    o.u32(76, x.flags);
    o.u32(80, x.portal_count);
    o.i32(84, x.floor_id);
    o.i32(88, x.exterior_visibility_depth);
    o.arr(96, mb.add_uint_array(x.attached_objects));
    return o.b;
}

MloPortal read_portal(const Meta& m, In r) {
    MloPortal x;
    x.room_from = r.u32(8);
    x.room_to = r.u32(12);
    x.flags = r.u32(16);
    x.mirror_priority = r.u32(20);
    x.opacity = r.u32(24);
    x.audio_occlusion = r.u32(28);
    MetaArray c = r.arr(32);
    if (c.pointer && c.count) {
        In cr{m.at(c.pointer, 16ull * c.count)};
        for (uint32_t i = 0; i < c.count; i++) { x.corners.push_back(cr.v3(16ull * i)); x.corner_padding.push_back(cr.f32(16ull * i + 12)); }
    }
    x.attached_objects = read_uints(m, r.arr(48));
    x.raw.assign(r.p, r.p + kPortal);
    return x;
}
Bytes write_portal(MetaBuilder& mb, const MloPortal& x) {
    Out o(kPortal, x.raw);
    o.u32(8, x.room_from);
    o.u32(12, x.room_to);
    o.u32(16, x.flags);
    o.u32(20, x.mirror_priority);
    o.u32(24, x.opacity);
    o.u32(28, x.audio_occlusion);
    o.arr(32, mb.add_padded_vec3_array(x.corners, x.corner_padding));
    o.arr(48, mb.add_uint_array(x.attached_objects));
    return o.b;
}

MloEntitySet read_entity_set(const Meta& m, In r) {
    MloEntitySet x;
    x.name = r.u32(8);
    x.locations = read_uints(m, r.arr(16));
    x.entities = read_entities(m, r.arr(32));
    x.raw.assign(r.p, r.p + kEntitySet);
    return x;
}
Bytes write_entity_set(MetaBuilder& mb, const MloEntitySet& x) {
    Out o(kEntitySet, x.raw);
    o.u32(8, x.name);
    o.arr(16, mb.add_uint_array(x.locations));
    o.arr(32, write_entities(mb, x.entities));
    return o.b;
}

Archetype read_archetype(const Meta& m, uint64_t ptr) {
    const MetaBlock& blk = m.block(meta_block_id(ptr));
    const MetaStruct* info = m.find_struct(blk.type);
    if (!info) info = known_struct(blk.type);
    size_t size = info ? (size_t)info->size : 0;
    Archetype a;
    if (blk.type == N().CBaseArchetypeDef && size == kBaseArch) a.kind = Archetype::Kind::Base;
    else if (blk.type == N().CTimeArchetypeDef && size == kTimeArch) a.kind = Archetype::Kind::Time;
    else if (blk.type == N().CMloArchetypeDef && size == kMloArch) a.kind = Archetype::Kind::Mlo;
    else {
        a.kind = Archetype::Kind::Other;
        a.other = detach_object(m, ptr);
        return a;
    }
    In r{m.at(ptr, size)};
    a.lod_dist = r.f32(8);
    a.flags = r.u32(12);
    a.special_attribute = r.u32(16);
    a.bb_min = r.v3(32);
    a.bb_max = r.v3(48);
    a.bs_centre = r.v3(64);
    a.bs_radius = r.f32(80);
    a.hd_texture_dist = r.f32(84);
    a.name = r.u32(88);
    a.texture_dictionary = r.u32(92);
    a.clip_dictionary = r.u32(96);
    a.drawable_dictionary = r.u32(100);
    a.physics_dictionary = r.u32(104);
    a.asset_type = r.i32(108);
    a.asset_name = r.u32(112);
    a.extensions = read_pointer_objects(m, r.arr(120));
    a.raw.assign(r.p, r.p + size);
    if (a.kind == Archetype::Kind::Time) a.time_flags = r.u32(144);
    if (a.kind == Archetype::Kind::Mlo) {
        a.mlo_flags = r.u32(144);
        a.entities = read_entities(m, r.arr(152));
        MetaArray rooms = r.arr(168), portals = r.arr(184), sets = r.arr(200), tcm = r.arr(216);
        if (rooms.pointer && rooms.count) {
            const uint8_t* p = m.at(rooms.pointer, kRoom * rooms.count);
            for (uint32_t i = 0; i < rooms.count; i++) a.rooms.push_back(read_room(m, In{p + kRoom * i}));
        }
        if (portals.pointer && portals.count) {
            const uint8_t* p = m.at(portals.pointer, kPortal * portals.count);
            for (uint32_t i = 0; i < portals.count; i++) a.portals.push_back(read_portal(m, In{p + kPortal * i}));
        }
        if (sets.pointer && sets.count) {
            const uint8_t* p = m.at(sets.pointer, kEntitySet * sets.count);
            for (uint32_t i = 0; i < sets.count; i++) a.entity_sets.push_back(read_entity_set(m, In{p + kEntitySet * i}));
        }
        if (tcm.pointer && tcm.count) {
            const uint8_t* p = m.at(tcm.pointer, kMloTcm * tcm.count);
            for (uint32_t i = 0; i < tcm.count; i++) {
                In t{p + kMloTcm * i};
                MloTimeCycleModifier x;
                x.name = t.u32(8); x.sphere = t.v4(16); x.percentage = t.f32(32); x.range = t.f32(36);
                x.start_hour = t.u32(40); x.end_hour = t.u32(44);
                x.raw.assign(t.p, t.p + kMloTcm);
                a.time_cycle_modifiers.push_back(x);
            }
        }
    }
    return a;
}

/// Writes one archetype (children first) and returns the pointer to its item.
uint64_t write_archetype(MetaBuilder& mb, const Archetype& a) {
    if (a.kind == Archetype::Kind::Other) return mb.add_object(a.other).pointer();
    size_t size = a.kind == Archetype::Kind::Base ? kBaseArch : a.kind == Archetype::Kind::Time ? kTimeArch : kMloArch;
    Out o(size, a.raw);
    o.f32(8, a.lod_dist);
    o.u32(12, a.flags);
    o.u32(16, a.special_attribute);
    o.v3(32, a.bb_min);
    o.v3(48, a.bb_max);
    o.v3(64, a.bs_centre);
    o.f32(80, a.bs_radius);
    o.f32(84, a.hd_texture_dist);
    o.u32(88, a.name);
    o.u32(92, a.texture_dictionary);
    o.u32(96, a.clip_dictionary);
    o.u32(100, a.drawable_dictionary);
    o.u32(104, a.physics_dictionary);
    o.i32(108, a.asset_type);
    o.u32(112, a.asset_name);
    o.arr(120, mb.add_object_pointers(a.extensions));
    uint32_t type = N().CBaseArchetypeDef;
    if (a.kind == Archetype::Kind::Time) {
        type = N().CTimeArchetypeDef;
        o.u32(144, a.time_flags);
    } else if (a.kind == Archetype::Kind::Mlo) {
        type = N().CMloArchetypeDef;
        o.u32(144, a.mlo_flags);
        o.arr(152, write_entities(mb, a.entities));
        auto array_of = [&](uint32_t t, size_t n, auto&& each) {
            if (n == 0) return MetaPtr{};
            Bytes all;
            for (size_t i = 0; i < n; i++) { Bytes b = each(i); all.insert(all.end(), b.begin(), b.end()); }
            return mb.add_item_array(t, all, (uint32_t)n);
        };
        o.arr(168, array_of(N().CMloRoomDef, a.rooms.size(), [&](size_t i) { return write_room(mb, a.rooms[i]); }));
        o.arr(184, array_of(N().CMloPortalDef, a.portals.size(), [&](size_t i) { return write_portal(mb, a.portals[i]); }));
        o.arr(200, array_of(N().CMloEntitySet, a.entity_sets.size(), [&](size_t i) { return write_entity_set(mb, a.entity_sets[i]); }));
        o.arr(216, array_of(N().CMloTimeCycleModifier, a.time_cycle_modifiers.size(), [&](size_t i) {
            auto& x = a.time_cycle_modifiers[i];
            Out t(kMloTcm, x.raw);
            t.u32(8, x.name); t.v4(16, x.sphere); t.f32(32, x.percentage); t.f32(36, x.range);
            t.u32(40, x.start_hour); t.u32(44, x.end_hour);
            return t.b;
        }));
    }
    return mb.add_item(type, o.b).pointer();
}

const MetaBlock& root_block(const Meta& m, uint32_t type) {
    const MetaBlock* r = m.root();
    if (r && r->type == type) return *r;
    if (const MetaBlock* b = m.find_block(type)) return *b;
    fail("no " + hash_name(type) + " block in the file");
}

} // namespace

Archetype* MapTypes::find(uint32_t h) {
    for (auto& a : archetypes) if (a.name == h) return &a;
    return nullptr;
}
const Archetype* MapTypes::find(uint32_t h) const {
    for (auto& a : archetypes) if (a.name == h) return &a;
    return nullptr;
}

// --- ytyp ---------------------------------------------------------------------------------------

MapTypes map_types_from_meta(const Meta& m) {
    const MetaBlock& rb = root_block(m, N().CMapTypes);
    if (rb.data.size() < kMapTypes) fail("CMapTypes block too small");
    In r{rb.data.data()};
    MapTypes t;
    t.structs = m.structs;
    t.enums = m.enums;
    t.extensions = read_pointer_objects(m, r.arr(8));
    MetaArray arch = r.arr(24);
    if (arch.pointer && arch.count) {
        const uint8_t* p = m.at(arch.pointer, 8ull * arch.count);
        for (uint32_t i = 0; i < arch.count; i++) {
            uint64_t ptr; std::memcpy(&ptr, p + 8ull * i, 8);
            t.archetypes.push_back(read_archetype(m, ptr));
        }
    }
    t.name = r.u32(40);
    t.dependencies = read_uints(m, r.arr(48));
    t.composite_entity_types = read_struct_objects(m, r.arr(64), N().CCompositeEntityType);
    t.raw.assign(r.p, r.p + kMapTypes);
    return t;
}

Meta to_meta(const MapTypes& t) {
    MetaBuilder mb;
    mb.reserve_block(N().CMapTypes);
    Out o(kMapTypes, t.raw);
    o.arr(8, mb.add_object_pointers(t.extensions));
    {
        std::vector<uint64_t> ptrs;
        for (auto& a : t.archetypes) ptrs.push_back(write_archetype(mb, a));
        o.arr(24, ptrs.empty() ? MetaPtr{} : mb.add_pointer_array(ptrs));
    }
    o.u32(40, t.name);
    o.arr(48, mb.add_uint_array(t.dependencies, metatype::kHash));
    o.arr(64, mb.add_object_array(N().CCompositeEntityType, t.composite_entity_types));
    mb.add_item(N().CMapTypes, o.b);
    return mb.finish("", &t.structs, &t.enums);
}

MapTypes read_ytyp(std::span<const uint8_t> file) { return map_types_from_meta(read_meta(file)); }
Bytes write_ytyp(const MapTypes& t, bool compress) { return write_meta(to_meta(t), compress); }

// --- ymap ---------------------------------------------------------------------------------------

namespace {
MetaObject detach_inline(const Meta& m, uint32_t type, const In& root, size_t at, size_t avail) {
    return detach_object(m, type, root.p + at, avail - at);
}
void put_inline(MetaBuilder& mb, Out& o, size_t at, size_t size, const MetaObject& obj) {
    std::memset(o.b.data() + at, 0, size); // drop stale bytes from `raw`
    if (obj.data.empty()) return;
    Bytes b = mb.pack_object(obj);
    std::memcpy(o.b.data() + at, b.data(), b.size());
}
} // namespace

MapData map_data_from_meta(const Meta& m) {
    const MetaBlock& rb = root_block(m, N().CMapData);
    if (rb.data.size() < kMapData) fail("CMapData block too small");
    In r{rb.data.data()};
    MapData d;
    d.structs = m.structs;
    d.enums = m.enums;
    d.name = r.u32(8);
    d.parent = r.u32(12);
    d.flags = r.u32(16);
    d.content_flags = r.u32(20);
    d.streaming_extents_min = r.v3(32);
    d.streaming_extents_max = r.v3(48);
    d.entities_extents_min = r.v3(64);
    d.entities_extents_max = r.v3(80);
    d.entities = read_entities(m, r.arr(96));
    d.container_lods = read_struct_objects(m, r.arr(112), N().rage__fwContainerLodDef);
    MetaArray box = r.arr(128);
    if (box.pointer && box.count) {
        In b{m.at(box.pointer, kBoxOccluder * box.count)};
        for (uint32_t i = 0; i < box.count; i++) {
            BoxOccluder x;
            x.center_x = b.get<int16_t>(16 * i); x.center_y = b.get<int16_t>(16 * i + 2); x.center_z = b.get<int16_t>(16 * i + 4);
            x.cos_z = b.get<int16_t>(16 * i + 6); x.length = b.get<int16_t>(16 * i + 8); x.width = b.get<int16_t>(16 * i + 10);
            x.height = b.get<int16_t>(16 * i + 12); x.sin_z = b.get<int16_t>(16 * i + 14);
            d.box_occluders.push_back(x);
        }
    }
    d.occlude_models = read_struct_objects(m, r.arr(144), N().OccludeModel);
    d.physics_dictionaries = read_uints(m, r.arr(160));
    d.instanced_data = detach_inline(m, N().rage__fwInstancedMapData, r, 176, kMapData);
    MetaArray tcm = r.arr(224);
    if (tcm.pointer && tcm.count) {
        const uint8_t* p = m.at(tcm.pointer, kTcm * tcm.count);
        for (uint32_t i = 0; i < tcm.count; i++) {
            In t{p + kTcm * i};
            TimeCycleModifier x;
            x.name = t.u32(8); x.min_extents = t.v3(16); x.max_extents = t.v3(32); x.percentage = t.f32(48);
            x.range = t.f32(52); x.start_hour = t.u32(56); x.end_hour = t.u32(60);
            x.raw.assign(t.p, t.p + kTcm);
            d.time_cycle_modifiers.push_back(x);
        }
    }
    MetaArray cg = r.arr(240);
    if (cg.pointer && cg.count) {
        const uint8_t* p = m.at(cg.pointer, kCarGen * cg.count);
        for (uint32_t i = 0; i < cg.count; i++) {
            In c{p + kCarGen * i};
            CarGenerator x;
            x.position = c.v3(16); x.orient_x = c.f32(32); x.orient_y = c.f32(36); x.perpendicular_length = c.f32(40);
            x.car_model = c.u32(44); x.flags = c.u32(48);
            for (int k = 0; k < 4; k++) x.body_color_remap[k] = c.i32(52 + 4 * k);
            x.pop_group = c.u32(68); x.livery = c.get<int8_t>(72);
            x.raw.assign(c.p, c.p + kCarGen);
            d.car_generators.push_back(x);
        }
    }
    d.lod_lights = detach_inline(m, N().CLODLight, r, 256, kMapData);
    d.distant_lod_lights = detach_inline(m, N().CDistantLODLight, r, 392, kMapData);
    d.block.version = r.u32(440);
    d.block.flags = r.u32(444);
    d.block.name = read_string(m, r.arr(448));
    d.block.exported_by = read_string(m, r.arr(464));
    d.block.owner = read_string(m, r.arr(480));
    d.block.time = read_string(m, r.arr(496));
    d.raw.assign(r.p, r.p + kMapData);
    return d;
}

Meta to_meta(const MapData& d) {
    MetaBuilder mb;
    mb.reserve_block(N().CMapData);
    Out o(kMapData, d.raw);
    o.u32(8, d.name);
    o.u32(12, d.parent);
    o.u32(16, d.flags);
    o.u32(20, d.content_flags);
    o.v3(32, d.streaming_extents_min);
    o.v3(48, d.streaming_extents_max);
    o.v3(64, d.entities_extents_min);
    o.v3(80, d.entities_extents_max);
    o.arr(96, write_entities(mb, d.entities));
    o.arr(112, mb.add_object_array(N().rage__fwContainerLodDef, d.container_lods));
    o.arr(128, MetaPtr{});
    if (!d.box_occluders.empty()) {
        Bytes all(d.box_occluders.size() * kBoxOccluder);
        std::memcpy(all.data(), d.box_occluders.data(), all.size()); // eight int16, no padding
        o.arr(128, mb.add_item_array(N().BoxOccluder, all, (uint32_t)d.box_occluders.size()));
    }
    o.arr(144, mb.add_object_array(N().OccludeModel, d.occlude_models));
    o.arr(160, mb.add_uint_array(d.physics_dictionaries, metatype::kHash));
    put_inline(mb, o, 176, 48, d.instanced_data);
    o.arr(224, MetaPtr{});
    if (!d.time_cycle_modifiers.empty()) {
        Bytes all;
        for (auto& x : d.time_cycle_modifiers) {
            Out t(kTcm, x.raw);
            t.u32(8, x.name); t.v3(16, x.min_extents); t.v3(32, x.max_extents); t.f32(48, x.percentage);
            t.f32(52, x.range); t.u32(56, x.start_hour); t.u32(60, x.end_hour);
            all.insert(all.end(), t.b.begin(), t.b.end());
        }
        o.arr(224, mb.add_item_array(N().CTimeCycleModifier, all, (uint32_t)d.time_cycle_modifiers.size()));
    }
    o.arr(240, MetaPtr{});
    if (!d.car_generators.empty()) {
        Bytes all;
        for (auto& x : d.car_generators) {
            Out c(kCarGen, x.raw);
            c.v3(16, x.position); c.f32(32, x.orient_x); c.f32(36, x.orient_y); c.f32(40, x.perpendicular_length);
            c.u32(44, x.car_model); c.u32(48, x.flags);
            for (int k = 0; k < 4; k++) c.i32(52 + 4 * k, x.body_color_remap[k]);
            c.u32(68, x.pop_group); c.put<int8_t>(72, x.livery);
            all.insert(all.end(), c.b.begin(), c.b.end());
        }
        o.arr(240, mb.add_item_array(N().CCarGen, all, (uint32_t)d.car_generators.size()));
    }
    put_inline(mb, o, 256, 136, d.lod_lights);
    put_inline(mb, o, 392, 48, d.distant_lod_lights);
    o.u32(440, d.block.version);
    o.u32(444, d.block.flags);
    o.arr(448, d.block.name.empty() ? MetaPtr{} : mb.add_string(d.block.name));
    o.arr(464, d.block.exported_by.empty() ? MetaPtr{} : mb.add_string(d.block.exported_by));
    o.arr(480, d.block.owner.empty() ? MetaPtr{} : mb.add_string(d.block.owner));
    o.arr(496, d.block.time.empty() ? MetaPtr{} : mb.add_string(d.block.time));
    mb.add_item(N().CMapData, o.b);
    return mb.finish("", &d.structs, &d.enums);
}

MapData read_ymap(std::span<const uint8_t> file) { return map_data_from_meta(read_meta(file)); }
Bytes write_ymap(const MapData& d, bool compress) { return write_meta(to_meta(d), compress); }

} // namespace rk
