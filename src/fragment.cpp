#include "ragekit/fragment.h"
#include "ragekit/error.h"
#include "drawable_io.h"
#include "bounds_io.h"
#include <cstring>

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

FragDrawable read_frag_drawable(const Reader& r, uint64_t ptr) {
    FragDrawable d;
    read_drawable_base(r, ptr, d);
    auto c = r.cursor(ptr + 0xB0);
    d.frag_matrix = c.r.at<BoundTransform>(c.pos); c.skip(64);
    uint64_t bound_ptr = c.u64();          // 0xF0
    uint64_t inds_ptr = c.u64();           // 0xF8
    uint16_t inds_count = c.u16();         // 0x100
    uint16_t cap = c.u16();
    c.u32();
    uint64_t mats_ptr = c.u64();           // 0x108
    d.frag_matrices_count = c.u16();       // 0x110
    uint64_t name_ptr = r.u64(ptr + 0x130);
    d.bound = read_bound(r, bound_ptr);
    d.frag_matrices_inds = r.array<uint64_t>(inds_ptr, inds_count);
    d.frag_matrices = r.array<BoundTransform>(mats_ptr, cap);
    d.name = r.str(name_ptr);
    return d;
}

FragPhysicsLod read_physics_lod(const Reader& r, uint64_t ptr) {
    FragPhysicsLod lod;
    std::memcpy(lod.raw.data(), r.resource().at(ptr, 304), 304);
    auto c = r.cursor(ptr + 0x20);
    uint64_t artic_ptr = c.u64();          // 0x20
    uint64_t unkfloats_ptr = c.u64();      // 0x28
    c.pos = ptr + 0xC0;
    uint64_t names_ptr = c.u64();          // 0xC0
    uint64_t groups_ptr = c.u64();         // 0xC8
    uint64_t children_ptr = c.u64();       // 0xD0
    uint64_t arch1_ptr = c.u64();          // 0xD8
    uint64_t arch2_ptr = c.u64();          // 0xE0
    uint64_t bound_ptr = c.u64();          // 0xE8
    uint64_t inertia_ptr = c.u64();        // 0xF0
    uint64_t unkvecs_ptr = c.u64();        // 0xF8
    uint64_t transforms_ptr = c.u64();     // 0x100
    uint64_t unk1_ptr = c.u64();           // 0x108
    uint64_t unk2_ptr = c.u64();           // 0x110
    uint8_t unk1_count = c.u8();           // 0x118
    uint8_t unk2_count = c.u8();
    uint8_t groups_count = c.u8();
    c.u8(); c.u8();
    uint8_t children_count = c.u8();       // 0x11D
    (void)names_ptr;

    if (artic_ptr) lod.articulated_body = r.bytes(artic_ptr, 176);
    auto gptrs = r.array<uint64_t>(groups_ptr, groups_count);
    for (auto gp : gptrs) {
        FragGroup g;
        if (gp) std::memcpy(g.raw.data(), r.resource().at(gp, 176), 176);
        lod.groups.push_back(g);
    }
    auto unkfloats = r.array<float>(unkfloats_ptr, children_count);
    auto inertias = r.array<Vec4>(inertia_ptr, children_count);
    auto unkvecs = r.array<Vec4>(unkvecs_ptr, children_count);
    auto cptrs = r.array<uint64_t>(children_ptr, children_count);
    for (size_t i = 0; i < cptrs.size(); i++) {
        FragChild ch;
        uint64_t cp = cptrs[i];
        if (cp) {
            auto cc = r.cursor(cp + 8);
            ch.pristine_mass = cc.f32();
            ch.damaged_mass = cc.f32();
            ch.group_index = cc.u16();
            ch.bone_tag = cc.u16();
            uint64_t d1 = r.u64(cp + 0xA0), d2 = r.u64(cp + 0xA8), ev = r.u64(cp + 0xB0);
            if (d1) ch.drawable = read_frag_drawable(r, d1);
            if (d2) ch.drawable_damaged = read_frag_drawable(r, d2);
            ch.has_event_set = ev != 0;
        }
        if (i < unkfloats.size()) ch.unk_float = unkfloats[i];
        if (i < inertias.size()) ch.inertia_tensor = inertias[i];
        if (i < unkvecs.size()) ch.unk_vec = unkvecs[i];
        lod.children.push_back(std::move(ch));
    }
    auto read_arch = [&](uint64_t ap) {
        FragArchetype a;
        std::memcpy(a.raw.data(), r.resource().at(ap, 224), 224);
        a.name = r.str(r.u64(ap + 0x18));
        a.bound = read_bound(r, r.u64(ap + 0x20));
        return a;
    };
    if (arch1_ptr) lod.archetype = read_arch(arch1_ptr);
    if (arch2_ptr) lod.archetype_damaged = read_arch(arch2_ptr);
    if (bound_ptr) {
        if (lod.archetype && r.u64(arch1_ptr + 0x20) == bound_ptr) lod.bound = lod.archetype->bound;
        else lod.bound = read_bound(r, bound_ptr);
    }
    if (transforms_ptr) {
        uint32_t count = r.u32(transforms_ptr + 0x10);
        lod.transforms = r.array<Mat4>(transforms_ptr + 0x20, count);
    }
    lod.unknown_data1 = r.bytes(unk1_ptr, unk1_count);
    lod.unknown_data2 = r.bytes(unk2_ptr, unk2_count);
    return lod;
}

} // namespace

Fragment read_yft(std::span<const uint8_t> file) {
    rsc::Resource res = rsc::Resource::load(file);
    Reader r(res);
    Fragment f;
    uint64_t ptr = rsc::kSystemBase;
    auto c = r.cursor(ptr + 0x20);
    f.sphere_center = c.vec3();
    f.sphere_radius = c.f32();
    uint64_t drawable_ptr = c.u64();       // 0x30
    uint64_t array_ptr = c.u64();          // 0x38
    uint64_t names_ptr = c.u64();          // 0x40
    uint32_t array_count = c.u32();        // 0x48
    c.u32(); c.u64();
    uint64_t name_ptr = c.u64();           // 0x58
    c.u64();                               // 0x60 cloths pointer
    f.cloth_count = c.u16();               // 0x68
    c.pos = ptr + 0xA8;
    uint64_t bone_tr_ptr = c.u64();        // 0xA8
    f.unkB0 = (int32_t)c.u32(); c.u32();
    f.unkB8 = (int32_t)c.u32();
    f.unkBC = (int32_t)c.u32();
    f.unkC0 = (int32_t)c.u32();
    f.unkC4 = (int32_t)c.u32();
    c.u32();                               // 0xC8 = -1
    f.unkCC = c.f32();
    f.gravity_factor = c.f32();
    f.buoyancy_factor = c.f32();
    c.u8();
    uint8_t glass_count = c.u8();          // 0xD9
    c.u16(); c.u32();
    uint64_t glass_ptr = c.u64();          // 0xE0
    c.u64();
    uint64_t phys_ptr = c.u64();           // 0xF0
    uint64_t cloth_drawable_ptr = c.u64(); // 0xF8
    c.u64(); c.u64();
    uint64_t lights_ptr = c.u64();         // 0x110
    uint16_t lights_count = c.u16();
    c.u16(); c.u32();
    uint64_t vglass_ptr = c.u64();         // 0x120

    f.name = r.str(name_ptr);
    if (drawable_ptr) f.drawable = read_frag_drawable(r, drawable_ptr);
    auto aptrs = r.array<uint64_t>(array_ptr, array_count);
    auto nptrs = r.array<uint64_t>(names_ptr, array_count);
    for (size_t i = 0; i < aptrs.size(); i++) {
        if (aptrs[i]) f.drawable_array.push_back(read_frag_drawable(r, aptrs[i]));
        else f.drawable_array.push_back(FragDrawable{});
        f.drawable_array_names.push_back(i < nptrs.size() ? r.str(nptrs[i]) : "");
    }
    if (bone_tr_ptr) {
        uint8_t n = r.u8(bone_tr_ptr + 0x10);
        f.bone_transforms_unk = r.u16(bone_tr_ptr + 0x12);
        f.bone_transforms = r.array<std::array<float, 12>>(bone_tr_ptr + 0x20, n);
    }
    auto gptrs = r.array<uint64_t>(glass_ptr, glass_count);
    for (auto gp : gptrs) {
        std::array<uint8_t, 112> g{};
        if (gp) std::memcpy(g.data(), r.resource().at(gp, 112), 112);
        f.glass_windows.push_back(g);
    }
    if (phys_ptr) {
        for (int i = 0; i < 3; i++) {
            uint64_t lp = r.u64(phys_ptr + 0x10 + 8 * i);
            if (lp) f.physics[i] = read_physics_lod(r, lp);
        }
    }
    if (cloth_drawable_ptr) f.drawable_cloth = read_frag_drawable(r, cloth_drawable_ptr);
    if (vglass_ptr) {
        uint32_t total = r.u32(vglass_ptr + 8);
        f.vehicle_glass_windows = r.bytes(vglass_ptr, total);
    }
    for (uint16_t i = 0; i < lights_count; i++) f.lights.push_back(read_light_at(r, lights_ptr + (uint64_t)i * 168));
    return f;
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
