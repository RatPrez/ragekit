#include "ragekit/fragment.h"
#include "ragekit/error.h"
#include "drawable_io.h"
#include "bounds_io.h"
#include "cloth_io.h"
#include <cstring>
#include <map>

namespace rk {

std::string FragGroup::name() const {
    std::string s((const char*)raw.data() + 0x80, 40);
    auto n = s.find('\0');
    return n == std::string::npos ? s : s.substr(0, n);
}
float FragGroup::mass() const { float f; std::memcpy(&f, raw.data() + 0x44, 4); return f; }
float FragGroup::strength() const { float f; std::memcpy(&f, raw.data() + 0x10, 4); return f; }
float FragArchetype::mass() const { float f; std::memcpy(&f, raw.data() + 0x40, 4); return f; }
Vec3 FragArchetype::inertia_tensor() const { Vec3 v; std::memcpy(&v, raw.data() + 0x60, 12); return v; }
Vec3 FragPhysicsLod::position_offset() const { Vec3 v; std::memcpy(&v, raw.data() + 0x30, 12); return v; }

namespace {

// The pointer fields of each structure: cleared in `raw` when read, set by the writer.
constexpr size_t kRootPointers[] = {0x08, 0x30, 0x38, 0x40, 0x58, 0x60, 0xA8, 0xE0, 0xF0, 0xF8, 0x110, 0x120};
constexpr size_t kLodPointers[] = {0x20, 0x28, 0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8, 0x100, 0x108, 0x110};
constexpr size_t kChildPointers[] = {0xA0, 0xA8, 0xB0};
constexpr size_t kArchetypePointers[] = {0x18, 0x20};
constexpr size_t kArticulatedPointers[] = {0x78, 0x80};

// The values the game's most common exporter build writes, for structures made from scratch (the game sets vtables itself).
constexpr uint32_t kRootVft = 1079456072u, kFragDrawableVft = 1080060904u, kLodGroupVft = 1080055504u, kLodVft = 1080055544u,
                   kChildVft = 1080061744u, kArchetypeVft = 1080215976u, kTransformsVft = 1080043568u, kGroupNamesVft = 1080043488u,
                   kArticulatedVft = 1080211704u;

template <size_t N, size_t M> void clear(std::array<uint8_t, N>& raw, const size_t (&offs)[M]) {
    for (size_t o : offs) std::memset(raw.data() + o, 0, 8);
}
template <size_t N> bool blank(const std::array<uint8_t, N>& raw) {
    for (uint8_t b : raw) if (b) return false;
    return true;
}
template <class T, size_t N> void set(std::array<uint8_t, N>& raw, size_t at, T v) { std::memcpy(raw.data() + at, &v, sizeof(T)); }
template <size_t N> std::array<uint8_t, N> read_raw(const Reader& r, uint64_t ptr) {
    std::array<uint8_t, N> a{};
    std::memcpy(a.data(), r.resource().at(ptr, N), N);
    return a;
}

std::vector<BoundTransform> read_transforms(const Reader& r, uint64_t ptr, size_t n) {
    std::vector<BoundTransform> v;
    for (auto& m : r.array<std::array<uint8_t, 64>>(ptr, n)) v.push_back(BoundTransform::from_file(m.data()));
    return v;
}

FragDrawable read_frag_drawable(const Reader& r, uint64_t ptr, std::map<uint64_t, BoundPtr>& bounds) {
    FragDrawable d;
    read_drawable_base(r, ptr, d);
    d.frag_matrix = BoundTransform::from_file(r.resource().at(ptr + 0xB0, 64));
    const uint64_t bound_ptr = r.u64(ptr + 0xF0), inds_ptr = r.u64(ptr + 0xF8), mats_ptr = r.u64(ptr + 0x108);
    const uint16_t inds_count = r.u16(ptr + 0x100), cap = r.u16(ptr + 0x102);
    d.frag_matrices_count = r.u16(ptr + 0x110);
    if (bound_ptr) {
        auto& b = bounds[bound_ptr];
        if (!b) b = read_bound(r, bound_ptr);
        d.bound = b;
    }
    d.frag_matrices_inds = r.array<uint64_t>(inds_ptr, inds_count);
    d.frag_matrices = read_transforms(r, mats_ptr, cap);
    d.name = r.str(r.u64(ptr + 0x130));
    return d;
}

FragPhysicsLod read_physics_lod(const Reader& r, uint64_t ptr, std::map<uint64_t, BoundPtr>& bounds) {
    FragPhysicsLod lod;
    lod.raw = read_raw<304>(r, ptr);
    auto p = [&](size_t at) { return r.u64(ptr + at); };
    const uint64_t artic_ptr = p(0x20), unkfloats_ptr = p(0x28), groups_ptr = p(0xC8), children_ptr = p(0xD0), arch1_ptr = p(0xD8),
                   arch2_ptr = p(0xE0), bound_ptr = p(0xE8), inertia_ptr = p(0xF0), unkvecs_ptr = p(0xF8), transforms_ptr = p(0x100),
                   unk1_ptr = p(0x108), unk2_ptr = p(0x110);
    const uint8_t unk1_count = r.u8(ptr + 0x118), unk2_count = r.u8(ptr + 0x119), groups_count = r.u8(ptr + 0x11A),
                  children_count = r.u8(ptr + 0x11D);
    clear(lod.raw, kLodPointers);
    auto bound = [&](uint64_t bp) -> BoundPtr {
        if (!bp) return nullptr;
        auto& b = bounds[bp];
        if (!b) b = read_bound(r, bp);
        return b;
    };

    if (artic_ptr) {
        FragArticulatedBody ab;
        ab.raw = read_raw<176>(r, artic_ptr);
        const uint64_t joints_ptr = r.u64(artic_ptr + 0x78), vectors_ptr = r.u64(artic_ptr + 0x80);
        const uint8_t vectors_count = r.u8(artic_ptr + 0x88), joints_count = r.u8(artic_ptr + 0x89);
        clear(ab.raw, kArticulatedPointers);
        for (uint64_t jp : r.array<uint64_t>(joints_ptr, joints_count)) {
            if (!jp) throw Error("yft: a null joint");
            const uint8_t type = r.u8(jp + 0x15);
            if (type > 1) throw Error("yft: joint type " + std::to_string(type) + " is not known");
            ab.joints.push_back(r.bytes(jp, type == 0 ? 176 : 240));
        }
        ab.vectors = r.array<Vec4>(vectors_ptr, vectors_count);
        lod.articulated_body = std::move(ab);
    }
    for (uint64_t gp : r.array<uint64_t>(groups_ptr, groups_count)) {
        if (!gp) throw Error("yft: a null physics group");
        FragGroup g;
        g.raw = read_raw<176>(r, gp);
        lod.groups.push_back(g);
    }
    const auto unkfloats = r.array<float>(unkfloats_ptr, children_count);
    const auto inertias = r.array<Vec4>(inertia_ptr, children_count);
    const auto unkvecs = r.array<Vec4>(unkvecs_ptr, children_count);
    const auto cptrs = r.array<uint64_t>(children_ptr, children_count);
    for (size_t i = 0; i < cptrs.size(); i++) {
        const uint64_t cp = cptrs[i];
        if (!cp) throw Error("yft: a null physics child");
        FragChild ch;
        ch.raw = read_raw<256>(r, cp);
        ch.pristine_mass = r.f32(cp + 0x08);
        ch.damaged_mass = r.f32(cp + 0x0C);
        ch.group_index = r.u16(cp + 0x10);
        ch.bone_tag = r.u16(cp + 0x12);
        const uint64_t d1 = r.u64(cp + 0xA0), d2 = r.u64(cp + 0xA8), ev = r.u64(cp + 0xB0);
        clear(ch.raw, kChildPointers);
        if (d1) ch.drawable = read_frag_drawable(r, d1, bounds);
        if (d2) ch.drawable_damaged = read_frag_drawable(r, d2, bounds);
        if (ev) ch.event_set = read_raw<48>(r, ev);
        if (i < unkfloats.size()) ch.unk_float = unkfloats[i];
        if (i < inertias.size()) ch.inertia_tensor = inertias[i];
        if (i < unkvecs.size()) ch.unk_vec = unkvecs[i];
        lod.children.push_back(std::move(ch));
    }
    auto read_arch = [&](uint64_t ap) {
        FragArchetype a;
        a.raw = read_raw<224>(r, ap);
        a.name = r.str(r.u64(ap + 0x18));
        a.bound = bound(r.u64(ap + 0x20));
        clear(a.raw, kArchetypePointers);
        return a;
    };
    if (arch1_ptr) lod.archetype = read_arch(arch1_ptr);
    if (arch2_ptr) lod.archetype_damaged = read_arch(arch2_ptr);
    lod.bound = bound(bound_ptr);
    if (transforms_ptr) lod.transforms = r.array<Mat4>(transforms_ptr + 0x20, r.u32(transforms_ptr + 0x10));
    lod.unknown_data1 = r.bytes(unk1_ptr, unk1_count);
    lod.unknown_data2 = r.bytes(unk2_ptr, unk2_count);
    return lod;
}

class Writer {
public:
    explicit Writer(Builder& b) : b_(b) {}

    BlockId bound(const BoundPtr& p) {
        if (!p) return {};
        auto it = bounds_.find(p.get());
        if (it != bounds_.end()) return it->second;
        const BlockId id = write_bound(b_, *p);
        bounds_.emplace(p.get(), id);
        return id;
    }
    BlockId str(const std::string& s) {
        if (s.empty()) return {};
        auto it = strings_.find(s);
        if (it != strings_.end()) return it->second;
        const BlockId id = b_.string(s);
        strings_.emplace(s, id);
        return id;
    }

    BlockId drawable(const FragDrawable& d) {
        if (d.frag_matrices_inds.size() > 0xFFFF || d.frag_matrices.size() > 0xFFFF) throw Error("yft: too many fragment matrices");
        const BlockId blk = b_.alloc(Segment::System, 336);
        const DrawableBlocks parts = prepare_drawable(b_, d);
        const BlockId inds = d.frag_matrices_inds.empty() ? BlockId{} : b_.array(Segment::System, d.frag_matrices_inds);
        BlockId mats;
        if (!d.frag_matrices.empty()) {
            mats = b_.alloc(Segment::System, d.frag_matrices.size() * 64);
            for (size_t i = 0; i < d.frag_matrices.size(); i++) d.frag_matrices[i].to_file(b_.data(mats) + i * 64);
        }
        const BlockId name = str(d.name), bnd = bound(d.bound);
        BlockWriter w(b_, blk);
        w.u32(kFragDrawableVft);
        w.u32(1);
        w.u64(0);
        write_drawable_base(w, d, parts, b_);
        d.frag_matrix.to_file(b_.data(blk) + 0xB0);
        w.seek(0xF0);
        w.ptr(bnd);
        w.ptr(inds);
        w.u16((uint16_t)d.frag_matrices_inds.size());
        w.u16((uint16_t)d.frag_matrices.size());
        w.u32(0);
        w.ptr(mats);
        w.u16(d.frag_matrices_count);
        w.u16(1);
        w.seek(0x130);
        w.ptr(name);
        return blk;
    }

    BlockId lod(const FragPhysicsLod& l) {
        if (l.groups.size() > 255 || l.children.size() > 255) throw Error("yft: a physics LOD holds at most 255 groups and 255 children");
        if (l.unknown_data1.size() > 255 || l.unknown_data2.size() > 255) throw Error("yft: unknown data of more than 255 bytes");
        auto raw = l.raw;
        if (blank(raw)) {
            set(raw, 0x00, kLodVft);
            set(raw, 0x04, 1u);
            for (size_t o : {0x3C, 0x4C}) set(raw, o, 0x7fc00001u);
            for (size_t o : {0x5C, 0x6C, 0x7C, 0x8C, 0x9C, 0xAC, 0xBC}) set(raw, o, 0x7f800001u);
            set(raw, 0x11C, uint8_t(1));
        }
        const uint8_t cc = (uint8_t)l.children.size();
        set(raw, 0x118, (uint8_t)l.unknown_data1.size());
        set(raw, 0x119, (uint8_t)l.unknown_data2.size());
        set(raw, 0x11A, (uint8_t)l.groups.size());
        set(raw, 0x11D, cc);
        set(raw, 0x11E, cc);
        const BlockId blk = b_.array(Segment::System, raw.data(), raw.size());

        std::vector<BlockId> groups;
        for (const FragGroup& g : l.groups) groups.push_back(b_.array(Segment::System, g.raw.data(), g.raw.size()));
        BlockId names;
        if (!groups.empty()) {
            names = b_.alloc(Segment::System, groups.size() * 8 + 8);
            for (size_t i = 0; i < groups.size(); i++) b_.pointer(names, i * 8, groups[i], 0x80);
            const uint32_t tail[2] = {kGroupNamesVft, 1};
            b_.put(names, groups.size() * 8, tail, 8);
        }
        std::vector<BlockId> children;
        std::vector<float> floats;
        std::vector<Vec4> inertias, vecs;
        for (const FragChild& c : l.children) {
            children.push_back(child(c));
            floats.push_back(c.unk_float);
            inertias.push_back(c.inertia_tensor);
            vecs.push_back(c.unk_vec);
        }
        const BlockId transforms = b_.alloc(Segment::System, 32 + l.transforms.size() * 64);
        {
            BlockWriter w(b_, transforms);
            w.u32(kTransformsVft);
            w.u32(1);
            w.u64(0);
            w.u32((uint32_t)l.transforms.size());
            w.seek(32);
            if (!l.transforms.empty()) w.bytes(l.transforms.data(), l.transforms.size() * 64);
        }

        b_.pointer(blk, 0x20, l.articulated_body ? articulated(*l.articulated_body) : BlockId{});
        b_.pointer(blk, 0x28, b_.array(Segment::System, floats));
        b_.pointer(blk, 0xC0, names);
        b_.pointer(blk, 0xC8, pointers(groups));
        b_.pointer(blk, 0xD0, pointers(children));
        b_.pointer(blk, 0xD8, l.archetype ? archetype(*l.archetype) : BlockId{});
        b_.pointer(blk, 0xE0, l.archetype_damaged ? archetype(*l.archetype_damaged) : BlockId{});
        b_.pointer(blk, 0xE8, bound(l.bound));
        b_.pointer(blk, 0xF0, b_.array(Segment::System, inertias));
        b_.pointer(blk, 0xF8, b_.array(Segment::System, vecs));
        b_.pointer(blk, 0x100, transforms);
        b_.pointer(blk, 0x108, b_.array(Segment::System, l.unknown_data1));
        b_.pointer(blk, 0x110, b_.array(Segment::System, l.unknown_data2));
        return blk;
    }

    BlockId pointers(const std::vector<BlockId>& blocks) {
        if (blocks.empty()) return {};
        const BlockId list = b_.alloc(Segment::System, blocks.size() * 8);
        for (size_t i = 0; i < blocks.size(); i++) b_.pointer(list, i * 8, blocks[i]);
        return list;
    }

private:
    BlockId child(const FragChild& c) {
        auto raw = c.raw;
        if (blank(raw)) {
            set(raw, 0x00, kChildVft);
            set(raw, 0x04, 1u);
        }
        set(raw, 0x08, c.pristine_mass);
        set(raw, 0x0C, c.damaged_mass);
        set(raw, 0x10, c.group_index);
        set(raw, 0x12, c.bone_tag);
        const BlockId blk = b_.array(Segment::System, raw.data(), raw.size());
        if (c.drawable) b_.pointer(blk, 0xA0, drawable(*c.drawable));
        if (c.drawable_damaged) b_.pointer(blk, 0xA8, drawable(*c.drawable_damaged));
        if (c.event_set) b_.pointer(blk, 0xB0, b_.array(Segment::System, c.event_set->data(), c.event_set->size()));
        return blk;
    }

    BlockId archetype(const FragArchetype& a) {
        auto raw = a.raw;
        if (blank(raw)) {
            set(raw, 0x00, kArchetypeVft);
            set(raw, 0x04, 1u);
        }
        const BlockId blk = b_.array(Segment::System, raw.data(), raw.size());
        b_.pointer(blk, 0x18, str(a.name));
        b_.pointer(blk, 0x20, bound(a.bound));
        return blk;
    }

    BlockId articulated(const FragArticulatedBody& ab) {
        if (ab.joints.size() > 255 || ab.vectors.size() > 255) throw Error("yft: an articulated body holds at most 255 joints and 255 vectors");
        auto raw = ab.raw;
        if (blank(raw)) {
            set(raw, 0x00, kArticulatedVft);
            set(raw, 0x04, 1u);
            set(raw, 0x6C, 1.0f);
        }
        set(raw, 0x88, (uint8_t)ab.vectors.size());
        set(raw, 0x89, (uint8_t)ab.joints.size());
        const BlockId blk = b_.array(Segment::System, raw.data(), raw.size());
        std::vector<BlockId> joints;
        for (const Bytes& j : ab.joints) {
            if (j.size() != 176 && j.size() != 240) throw Error("yft: a joint is 176 or 240 bytes");
            joints.push_back(b_.array(Segment::System, j));
        }
        b_.pointer(blk, 0x78, joints.empty() ? b_.alloc(Segment::System, 0) : pointers(joints));   // the game's always has the list
        b_.pointer(blk, 0x80, b_.array(Segment::System, ab.vectors));
        return blk;
    }

    Builder& b_;
    std::map<const Bound*, BlockId> bounds_;
    std::map<std::string, BlockId> strings_;
};

// A glass window: 64 bytes, a vertex declaration (Legacy 16 bytes, Enhanced 320), then 32 bytes. Fragment keeps Legacy's.
constexpr size_t kGlassHead = 0x40, kGlassTail = 32, kGlassSizeGen9 = kGlassHead + 320 + kGlassTail;

std::array<uint8_t, 112> read_glass_gen9(const Reader& r, uint64_t ptr) {
    std::array<uint8_t, 112> g{};
    const uint8_t* p = r.resource().at(ptr, kGlassSizeGen9);
    std::memcpy(g.data(), p, kGlassHead);
    uint16_t stride = 0;
    const VertexLayout l = layout_from_gen9(p + kGlassHead, stride);
    set(g, 0x40, l.flags);
    set(g, 0x44, stride);
    set(g, 0x47, l.count());
    set(g, 0x48, l.types);
    std::memcpy(g.data() + 0x50, p + kGlassHead + 320, kGlassTail);
    return g;
}

Bytes write_glass_gen9(const std::array<uint8_t, 112>& g, const std::array<uint8_t, 320>* read_decl) {
    Bytes out(kGlassSizeGen9);
    std::memcpy(out.data(), g.data(), kGlassHead);
    VertexLayout l;
    std::memcpy(&l.flags, g.data() + 0x40, 4);
    std::memcpy(&l.types, g.data() + 0x48, 8);
    bool same = false;
    if (read_decl) {
        // the declaration read keeps what a rebuilt one would not (some of the game's carry strides on unused slots)
        uint16_t stride = 0;
        const VertexLayout rl = layout_from_gen9(read_decl->data(), stride);
        same = rl.flags == l.flags && rl.types == layout_gen9(l).types;
    }
    if (same) std::memcpy(out.data() + kGlassHead, read_decl->data(), 320);
    else layout_to_gen9(layout_gen9(l), out.data() + kGlassHead);
    std::memcpy(out.data() + kGlassHead + 320, g.data() + 0x50, kGlassTail);
    return out;
}

} // namespace

Fragment read_yft(std::span<const uint8_t> file) {
    rsc::Resource res = rsc::Resource::load(file);
    Reader r(res, rsc::game_of("yft", res.version));
    Fragment f;
    std::map<uint64_t, BoundPtr> bounds;
    const uint64_t ptr = rsc::kSystemBase;
    f.raw = read_raw<304>(r, ptr);
    auto p = [&](size_t at) { return r.u64(ptr + at); };
    f.sphere_center = r.at<Vec3>(ptr + 0x20);
    f.sphere_radius = r.f32(ptr + 0x2C);
    const uint64_t drawable_ptr = p(0x30), array_ptr = p(0x38), names_ptr = p(0x40), name_ptr = p(0x58), cloths_ptr = p(0x60),
                   bone_tr_ptr = p(0xA8), glass_ptr = p(0xE0), phys_ptr = p(0xF0), cloth_drawable_ptr = p(0xF8), lights_ptr = p(0x110),
                   vglass_ptr = p(0x120);
    const uint32_t array_count = r.u32(ptr + 0x48);
    const uint16_t cloth_count = r.u16(ptr + 0x68), lights_count = r.u16(ptr + 0x118);
    const uint8_t glass_count = r.u8(ptr + 0xD9);
    f.unkB0 = (int32_t)r.u32(ptr + 0xB0);
    f.unkB8 = (int32_t)r.u32(ptr + 0xB8);
    f.unkBC = (int32_t)r.u32(ptr + 0xBC);
    f.unkC0 = (int32_t)r.u32(ptr + 0xC0);
    f.unkC4 = (int32_t)r.u32(ptr + 0xC4);
    f.unkCC = r.f32(ptr + 0xCC);
    f.gravity_factor = r.f32(ptr + 0xD0);
    f.buoyancy_factor = r.f32(ptr + 0xD4);
    clear(f.raw, kRootPointers);

    f.name = r.str(name_ptr);
    if (drawable_ptr) f.drawable = read_frag_drawable(r, drawable_ptr, bounds);
    const auto aptrs = r.array<uint64_t>(array_ptr, array_count);
    const auto nptrs = r.array<uint64_t>(names_ptr, array_count);
    for (size_t i = 0; i < aptrs.size(); i++) {
        f.drawable_array.push_back(aptrs[i] ? read_frag_drawable(r, aptrs[i], bounds) : FragDrawable{});
        f.drawable_array_names.push_back(i < nptrs.size() ? r.str(nptrs[i]) : "");
    }
    if (bone_tr_ptr) {
        f.bone_transforms_unk = r.u16(bone_tr_ptr + 0x12);
        f.bone_transforms = r.array<std::array<float, 12>>(bone_tr_ptr + 0x20, r.u8(bone_tr_ptr + 0x10));
    }
    for (uint64_t gp : r.array<uint64_t>(glass_ptr, glass_count)) {
        if (!gp) throw Error("yft: a null glass window");
        f.glass_windows.push_back(r.gen9() ? read_glass_gen9(r, gp) : read_raw<112>(r, gp));
        if (r.gen9()) f.glass_declarations_gen9.push_back(read_raw<320>(r, gp + kGlassHead));
    }
    if (phys_ptr)
        for (int i = 0; i < 3; i++)
            if (const uint64_t lp = r.u64(phys_ptr + 0x10 + 8 * i)) f.physics[(size_t)i] = read_physics_lod(r, lp, bounds);
    if (cloth_drawable_ptr) f.drawable_cloth = read_frag_drawable(r, cloth_drawable_ptr, bounds);
    for (uint64_t cp : r.array<uint64_t>(cloths_ptr, cloth_count)) {
        if (!cp) throw Error("yft: a null cloth");
        f.cloths.push_back(read_environment_cloth(r, cp));
    }
    if (vglass_ptr) f.vehicle_glass_windows = r.bytes(vglass_ptr, r.u32(vglass_ptr + 8));
    for (uint16_t i = 0; i < lights_count; i++) f.lights.push_back(read_light_at(r, lights_ptr + (uint64_t)i * 168));
    return f;
}

Bytes write_yft(const Fragment& f, Game game) {
    if (f.drawable_array.size() != f.drawable_array_names.size()) throw Error("yft: drawable_array and drawable_array_names differ in count");
    if (f.glass_windows.size() > 255 || f.bone_transforms.size() > 255) throw Error("yft: at most 255 glass windows and 255 bone transforms");
    if (f.cloths.size() > 0xFFFF || f.lights.size() > 0xFFFF) throw Error("yft: too many cloths or lights");
    if (!f.cloths.empty() && !f.drawable_cloth && !f.drawable) throw Error("yft: a cloth needs the fragment's drawable to draw through");
    std::vector<const Drawable*> all;
    if (f.drawable) all.push_back(&*f.drawable);
    if (f.drawable_cloth) all.push_back(&*f.drawable_cloth);
    for (auto& d : f.drawable_array) all.push_back(&d);
    for (auto& lod : f.physics)
        if (lod) for (auto& c : lod->children) { if (c.drawable) all.push_back(&*c.drawable); if (c.drawable_damaged) all.push_back(&*c.drawable_damaged); }
    Gen9SamplerScope samplers(all);
    Builder b;
    b.set_game(game);
    Writer w(b);
    auto raw = f.raw;
    if (blank(raw)) {
        set(raw, 0x00, kRootVft);
        set(raw, 0x04, 1u);
        set(raw, 0xC8, -1);
    }
    std::memcpy(raw.data() + 0x20, &f.sphere_center, 12);
    set(raw, 0x2C, f.sphere_radius);
    set(raw, 0x48, (uint32_t)f.drawable_array.size());
    set(raw, 0x4C, f.drawable_array.empty() ? -1 : 0);
    set(raw, 0x68, (uint16_t)f.cloths.size());
    set(raw, 0x6A, (uint16_t)f.cloths.size());
    set(raw, 0xB0, f.unkB0);
    set(raw, 0xB8, f.unkB8);
    set(raw, 0xBC, f.unkBC);
    set(raw, 0xC0, f.unkC0);
    set(raw, 0xC4, f.unkC4);
    set(raw, 0xCC, f.unkCC);
    set(raw, 0xD0, f.gravity_factor);
    set(raw, 0xD4, f.buoyancy_factor);
    set(raw, 0xD9, (uint8_t)f.glass_windows.size());
    set(raw, 0x118, (uint16_t)f.lights.size());
    set(raw, 0x11A, (uint16_t)f.lights.size());
    const BlockId root = b.array(Segment::System, raw.data(), raw.size());
    b.pointer(root, 0x08, b.alloc_pages_info());

    const BlockId drawable = f.drawable ? w.drawable(*f.drawable) : BlockId{};
    b.pointer(root, 0x30, drawable);
    std::vector<BlockId> array, names;
    for (size_t i = 0; i < f.drawable_array.size(); i++) {
        array.push_back(w.drawable(f.drawable_array[i]));
        names.push_back(w.str(f.drawable_array_names[i]));
    }
    b.pointer(root, 0x38, w.pointers(array));
    b.pointer(root, 0x40, w.pointers(names));
    b.pointer(root, 0x58, w.str(f.name));
    if (!f.bone_transforms.empty()) {
        const BlockId bt = b.alloc(Segment::System, 32 + f.bone_transforms.size() * 48);
        BlockWriter bw(b, bt);
        bw.seek(0x10);
        bw.u8((uint8_t)f.bone_transforms.size());
        bw.u8((uint8_t)f.bone_transforms.size());
        bw.u16(f.bone_transforms_unk);
        bw.seek(0x20);
        bw.bytes(f.bone_transforms.data(), f.bone_transforms.size() * 48);
        b.pointer(root, 0xA8, bt);
    }
    std::vector<BlockId> glass;
    for (size_t i = 0; i < f.glass_windows.size(); i++) {
        const auto& g = f.glass_windows[i];
        const auto* decl = i < f.glass_declarations_gen9.size() ? &f.glass_declarations_gen9[i] : nullptr;
        glass.push_back(b.gen9() ? b.array(Segment::System, write_glass_gen9(g, decl)) : b.array(Segment::System, g.data(), g.size()));
    }
    b.pointer(root, 0xE0, w.pointers(glass));
    if (f.physics[0] || f.physics[1] || f.physics[2]) {
        const BlockId group = b.alloc(Segment::System, 48);
        BlockWriter gw(b, group);
        gw.u32(kLodGroupVft);
        gw.u32(1);
        for (int i = 0; i < 3; i++)
            if (f.physics[(size_t)i]) b.pointer(group, 0x10 + 8 * (size_t)i, w.lod(*f.physics[(size_t)i]));
        b.pointer(root, 0xF0, group);
    }
    const BlockId cloth_drawable = f.drawable_cloth ? w.drawable(*f.drawable_cloth) : BlockId{};
    b.pointer(root, 0xF8, cloth_drawable);
    std::vector<BlockId> cloths;
    for (const EnvironmentCloth& c : f.cloths) cloths.push_back(write_environment_cloth(b, c, cloth_drawable.valid() ? cloth_drawable : drawable));
    b.pointer(root, 0x60, w.pointers(cloths));
    if (!f.lights.empty()) {
        const BlockId lights = b.alloc(Segment::System, f.lights.size() * 168);
        BlockWriter lw(b, lights);
        for (const Light& l : f.lights) write_light_at(lw, l);
        b.pointer(root, 0x110, lights);
    }
    if (!f.vehicle_glass_windows.empty()) b.pointer(root, 0x120, b.array(Segment::System, f.vehicle_glass_windows));
    return b.build(rsc::version_for("yft", game));
}

std::vector<Fragment::Part> Fragment::visual_parts() const {
    std::vector<Part> parts;
    const Drawable* main = drawable ? &*drawable : (drawable_cloth ? &*drawable_cloth : nullptr);
    if (main) parts.push_back({main, Mat4::identity(), name});
    const auto& lod = physics[0];
    if (!lod) return parts;
    const Skeleton* skel = main && main->skeleton ? &*main->skeleton : nullptr;
    for (size_t i = 0; i < lod->children.size(); i++) {
        auto& ch = lod->children[i];
        if (!ch.drawable) continue;
        Mat4 m = Mat4::identity();
        if (skel) {
            const Bone* bone = skel->find_tag(ch.bone_tag);
            if (bone) m = skel->bind_matrix((size_t)(bone - skel->bones.data()));
        }
        std::string pname = ch.group_index < lod->groups.size() ? lod->groups[ch.group_index].name() : "child" + std::to_string(i);
        parts.push_back({&*ch.drawable, m, pname});
    }
    return parts;
}

} // namespace rk
