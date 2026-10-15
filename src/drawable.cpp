#include "ragekit/drawable.h"
#include "ragekit/hash.h"
#include "ragekit/error.h"
#include "drawable_io.h"
#include "bounds_io.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace rk {

// ================================================================ model helpers

ShaderParam ShaderParam::tex(std::string_view name, const std::string& texture) {
    ShaderParam p;
    p.name_hash = joaat_lower(name);
    p.type = ParamType::Texture;
    p.texture = texture;
    return p;
}
ShaderParam ShaderParam::vec(std::string_view name, Vec4 v) {
    ShaderParam p;
    p.name_hash = joaat_lower(name);
    p.type = ParamType::Vector;
    p.values = {v};
    return p;
}

const ShaderParam* Shader::find(uint32_t h) const {
    for (auto& p : params) if (p.name_hash == h) return &p;
    return nullptr;
}
const ShaderParam* Shader::find(std::string_view n) const { return find(joaat_lower(n)); }
ShaderParam* Shader::find(std::string_view n) {
    uint32_t h = joaat_lower(n);
    for (auto& p : params) if (p.name_hash == h) return &p;
    return nullptr;
}
std::string Shader::texture_of(std::string_view sampler) const {
    auto* p = find(sampler);
    return (p && p->type == ParamType::Texture) ? p->texture : "";
}

uint32_t vertex_component_size(VertexComponentType t) {
    switch (t) {
        case VertexComponentType::Half2: case VertexComponentType::Float: case VertexComponentType::UByte4:
        case VertexComponentType::Colour: case VertexComponentType::RGBA8SNorm: return 4;
        case VertexComponentType::Half4: case VertexComponentType::Float2: return 8;
        case VertexComponentType::Float3: return 12;
        case VertexComponentType::Float4: return 16;
        default: return 0;
    }
}

uint32_t VertexLayout::offset_of(VertexSemantic s) const {
    uint32_t off = 0;
    for (int k = 0; k < (int)s; k++)
        if ((flags >> k) & 1) off += vertex_component_size(type_of((VertexSemantic)k));
    return off;
}
uint16_t VertexLayout::stride() const {
    uint32_t n = 0;
    for (int k = 0; k < 16; k++)
        if ((flags >> k) & 1) n += vertex_component_size(type_of((VertexSemantic)k));
    return (uint16_t)n;
}
uint8_t VertexLayout::count() const {
    uint8_t n = 0;
    for (int k = 0; k < 16; k++) if ((flags >> k) & 1) n++;
    return n;
}
VertexLayout VertexLayout::standard(std::initializer_list<VertexSemantic> sems) {
    VertexLayout l;
    for (auto s : sems) l.flags |= 1u << (int)s;
    return l;
}

Vec4 Geometry::get(uint32_t v, VertexSemantic s) const {
    Vec4 out;
    if (!layout.has(s) || v >= vertex_count) return out;
    const uint8_t* p = vertices.data() + (size_t)v * layout.stride() + layout.offset_of(s);
    auto f = [&](int i) { float x; std::memcpy(&x, p + i * 4, 4); return x; };
    auto h = [&](int i) { uint16_t x; std::memcpy(&x, p + i * 2, 2); return half_to_float(x); };
    switch (layout.type_of(s)) {
        case VertexComponentType::Float:  out.x = f(0); break;
        case VertexComponentType::Float2: out.x = f(0); out.y = f(1); break;
        case VertexComponentType::Float3: out.x = f(0); out.y = f(1); out.z = f(2); break;
        case VertexComponentType::Float4: out.x = f(0); out.y = f(1); out.z = f(2); out.w = f(3); break;
        case VertexComponentType::Half2:  out.x = h(0); out.y = h(1); break;
        case VertexComponentType::Half4:  out.x = h(0); out.y = h(1); out.z = h(2); out.w = h(3); break;
        case VertexComponentType::Colour:
        case VertexComponentType::UByte4:
            out = {p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f};
            break;
        case VertexComponentType::RGBA8SNorm:
            out = {(int8_t)p[0] / 127.f, (int8_t)p[1] / 127.f, (int8_t)p[2] / 127.f, (int8_t)p[3] / 127.f};
            break;
        default: break;
    }
    return out;
}

void Geometry::set(uint32_t v, VertexSemantic s, const Vec4& val) {
    if (!layout.has(s) || v >= vertex_count) return;
    uint8_t* p = vertices.data() + (size_t)v * layout.stride() + layout.offset_of(s);
    auto f = [&](int i, float x) { std::memcpy(p + i * 4, &x, 4); };
    auto h = [&](int i, float x) { uint16_t q = float_to_half(x); std::memcpy(p + i * 2, &q, 2); };
    auto b = [](float x) { return (uint8_t)std::lround(std::clamp(x, 0.f, 1.f) * 255.f); };
    auto sb = [](float x) { return (uint8_t)(int8_t)std::lround(std::clamp(x, -1.f, 1.f) * 127.f); };
    switch (layout.type_of(s)) {
        case VertexComponentType::Float:  f(0, val.x); break;
        case VertexComponentType::Float2: f(0, val.x); f(1, val.y); break;
        case VertexComponentType::Float3: f(0, val.x); f(1, val.y); f(2, val.z); break;
        case VertexComponentType::Float4: f(0, val.x); f(1, val.y); f(2, val.z); f(3, val.w); break;
        case VertexComponentType::Half2:  h(0, val.x); h(1, val.y); break;
        case VertexComponentType::Half4:  h(0, val.x); h(1, val.y); h(2, val.z); h(3, val.w); break;
        case VertexComponentType::Colour:
        case VertexComponentType::UByte4:
            p[0] = b(val.x); p[1] = b(val.y); p[2] = b(val.z); p[3] = b(val.w);
            break;
        case VertexComponentType::RGBA8SNorm:
            p[0] = sb(val.x); p[1] = sb(val.y); p[2] = sb(val.z); p[3] = sb(val.w);
            break;
        default: break;
    }
}

void Geometry::resize_vertices(uint32_t count) {
    vertex_count = count;
    vertices.assign((size_t)count * layout.stride(), 0);
}

void Geometry::compute_bounds() {
    Vec3 mn{INFINITY, INFINITY, INFINITY}, mx{-INFINITY, -INFINITY, -INFINITY};
    for (uint32_t i = 0; i < vertex_count; i++) {
        Vec3 p = position(i);
        mn = vmin(mn, p);
        mx = vmax(mx, p);
    }
    if (vertex_count == 0) mn = mx = {};
    bounds.min = Vec4(mn, 0);
    bounds.max = Vec4(mx, 0);
}

void Model::compute_bounds() {
    Vec3 mn{INFINITY, INFINITY, INFINITY}, mx{-INFINITY, -INFINITY, -INFINITY};
    for (auto& g : geometries) {
        mn = vmin(mn, g.bounds.min.xyz());
        mx = vmax(mx, g.bounds.max.xyz());
    }
    if (geometries.empty()) mn = mx = {};
    bounds.min = Vec4(mn, 0);
    bounds.max = Vec4(mx, 0);
}

std::vector<const Model*> Drawable::all_models() const {
    std::vector<const Model*> v;
    for (auto& lod : lods) for (auto& m : lod) v.push_back(&m);
    for (auto& m : lods_extra) v.push_back(&m);
    return v;
}

void Drawable::compute_bounds() {
    Vec3 mn{INFINITY, INFINITY, INFINITY}, mx{-INFINITY, -INFINITY, -INFINITY};
    bool any = false;
    for (int l = 0; l < 4; l++) {
        uint8_t mask = 0;
        for (auto& m : lods[l]) {
            for (auto& g : m.geometries) g.compute_bounds();
            m.compute_bounds();
            mask |= m.render_mask;
            if (!m.geometries.empty()) {
                mn = vmin(mn, m.bounds.min.xyz());
                mx = vmax(mx, m.bounds.max.xyz());
                any = true;
            }
        }
        render_mask_flags[l] = (render_mask_flags[l] & 0xFFFF00FFu) | ((uint32_t)mask << 8);
    }
    if (!any) mn = mx = {};
    bbox_min = mn;
    bbox_max = mx;
    bounding_center = (mn + mx) * 0.5f;
    bounding_radius = (mx - mn).length() * 0.5f;
}

const Drawable* DrawableDictionary::find(uint32_t h) const {
    for (size_t i = 0; i < hashes.size() && i < drawables.size(); i++)
        if (hashes[i] == h) return &drawables[i];
    return nullptr;
}

// ================================================================ skeleton maths

namespace {

Mat4 mat_from_quat_pos(const Quat& q, const Vec3& t) {
    // SharpDX / RAGE convention: row vectors, translation in row 4.
    Mat4 m;
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, zw = q.z * q.w, zx = q.z * q.x, yw = q.y * q.w, yz = q.y * q.z, xw = q.x * q.w;
    m.at(0, 0) = 1 - 2 * (yy + zz); m.at(0, 1) = 2 * (xy + zw);     m.at(0, 2) = 2 * (zx - yw);     m.at(0, 3) = 0;
    m.at(1, 0) = 2 * (xy - zw);     m.at(1, 1) = 1 - 2 * (zz + xx); m.at(1, 2) = 2 * (yz + xw);     m.at(1, 3) = 0;
    m.at(2, 0) = 2 * (zx + yw);     m.at(2, 1) = 2 * (yz - xw);     m.at(2, 2) = 1 - 2 * (yy + xx); m.at(2, 3) = 0;
    m.at(3, 0) = t.x; m.at(3, 1) = t.y; m.at(3, 2) = t.z; m.at(3, 3) = 1;
    return m;
}

Vec3 quat_rotate(const Quat& q, const Vec3& v) {
    // v' = v + 2w(q x v) + 2 q x (q x v)
    Vec3 u{q.x, q.y, q.z};
    Vec3 c1{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    Vec3 c2{u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x};
    return v + c1 * (2 * q.w) + c2 * 2;
}

Quat quat_mul(const Quat& a, const Quat& b) {
    // SharpDX Quaternion a*b (apply a then b in row-vector convention)
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
        a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Mat4 affine_inverse(const Mat4& m) {
    Mat4 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.at(i, j) = m.at(j, i);
    Vec3 t{m.at(3, 0), m.at(3, 1), m.at(3, 2)};
    for (int j = 0; j < 3; j++)
        r.at(3, j) = -(t.x * r.at(0, j) + t.y * r.at(1, j) + t.z * r.at(2, j));
    r.at(0, 3) = r.at(1, 3) = r.at(2, 3) = 0;
    r.at(3, 3) = 1;
    return r;
}

} // namespace

Mat4 Skeleton::bind_matrix(size_t bone) const {
    if (bone >= bones.size()) return Mat4::identity();
    Vec3 pos = bones[bone].translation;
    Quat ori = bones[bone].rotation;
    int16_t p = bones[bone].parent;
    int guard = 0;
    while (p >= 0 && (size_t)p < bones.size() && guard++ < 512) {
        pos = quat_rotate(bones[p].rotation, pos) + bones[p].translation;
        ori = quat_mul(bones[p].rotation, ori);
        p = bones[p].parent;
    }
    return mat_from_quat_pos(ori, pos);
}

const Bone* Skeleton::find_tag(uint16_t tag) const {
    for (auto& b : bones) if (b.tag == tag) return &b;
    return nullptr;
}

// ================================================================ reading

namespace {

VertexLayout read_layout(const Reader& r, uint64_t ptr) {
    VertexLayout l;
    if (ptr == 0) return l;
    l.flags = r.u32(ptr);
    l.types = r.u64(ptr + 8);
    return l;
}

Geometry read_geometry(const Reader& r, uint64_t ptr) {
    Geometry g;
    auto c = r.cursor(ptr + 0x18);
    uint64_t vb_ptr = c.u64();
    c.skip(24);
    uint64_t ib_ptr = c.u64();          // 0x38
    c.skip(24);
    uint32_t indices_count = c.u32();   // 0x58
    c.u32();                            // triangles
    uint16_t vertices_count = c.u16();  // 0x60
    c.u16(); c.u32();
    uint64_t bone_ids_ptr = c.u64();    // 0x68
    uint16_t stride = c.u16();          // 0x70
    uint16_t bone_ids_count = c.u16();
    g.bone_ids = r.array<uint16_t>(bone_ids_ptr, bone_ids_count);
    if (vb_ptr) {
        auto v = r.cursor(vb_ptr + 8);
        uint16_t vstride = v.u16();
        g.vb_flags = v.u16();
        v.u32();
        uint64_t data1 = v.u64();
        uint32_t vcount = v.u32();
        v.u32();
        uint64_t data2 = v.u64();
        v.u64();
        uint64_t info = v.u64();
        g.layout = read_layout(r, info);
        if (vstride == 0) vstride = stride;
        g.vertex_count = vcount ? vcount : vertices_count;
        uint64_t data = data1 ? data1 : data2;
        g.vb_data2_only = data1 == 0 && data2 != 0; // xml:
        g.vertices = r.bytes(data, (size_t)g.vertex_count * vstride);
        if (g.layout.stride() != vstride && g.layout.flags != 0)
            throw Error("geometry vertex stride does not match its layout");
    }
    if (ib_ptr) {
        uint32_t n = r.u32(ib_ptr + 8);
        uint64_t ip = r.u64(ib_ptr + 0x10);
        if (n == 0) n = indices_count;
        g.indices = r.array<uint16_t>(ip, n);
    }
    return g;
}

Model read_model(const Reader& r, uint64_t ptr) {
    Model m;
    auto c = r.cursor(ptr + 8);
    uint64_t geoms_ptr = c.u64();
    uint16_t count = c.u16();
    c.u16(); c.u32();
    uint64_t bounds_ptr = c.u64();      // 0x18
    uint64_t mapping_ptr = c.u64();     // 0x20
    m.skeleton_binding = c.u32();       // 0x28
    uint16_t rmf = c.u16();             // 0x2C
    m.render_mask = (uint8_t)(rmf & 0xFF);
    m.flags = (uint8_t)(rmf >> 8);
    auto mapping = r.array<uint16_t>(mapping_ptr, count);
    auto gptrs = r.array<uint64_t>(geoms_ptr, count);
    auto boxes = r.array<AABB>(bounds_ptr, count > 1 ? count + 1 : count);
    for (uint16_t i = 0; i < count; i++) {
        if (gptrs[i] == 0) continue;
        Geometry g = read_geometry(r, gptrs[i]);
        g.shader_index = i < mapping.size() ? mapping[i] : 0;
        if (!boxes.empty()) g.bounds = boxes.size() > 1 ? boxes[i + 1] : boxes[0];
        m.geometries.push_back(std::move(g));
    }
    if (!boxes.empty()) m.bounds = boxes[0];
    return m;
}

std::vector<Model> read_model_list(const Reader& r, uint64_t list_ptr) {
    std::vector<Model> out;
    if (list_ptr == 0) return out;
    uint64_t ptrs = r.u64(list_ptr);
    uint16_t count = r.u16(list_ptr + 8);
    auto arr = r.array<uint64_t>(ptrs, count);
    for (auto p : arr) if (p) out.push_back(read_model(r, p));
    return out;
}

Shader read_shader(const Reader& r, uint64_t ptr) {
    Shader s;
    auto c = r.cursor(ptr);
    uint64_t params_ptr = c.u64();
    s.name_hash = c.u32();
    c.u32();
    uint8_t count = c.u8();
    s.render_bucket = c.u8();
    s.unk12 = c.u16();
    c.u16(); c.u16();              // ParameterSize, ParameterDataSize
    s.sps_hash = c.u32();          // 0x18
    if (params_ptr && count) {
        std::vector<std::pair<uint8_t, uint64_t>> raw; // (type, ptr)
        std::vector<uint8_t> slots;
        uint64_t p = params_ptr;
        for (uint8_t i = 0; i < count; i++) {
            uint8_t type = r.u8(p);
            slots.push_back(r.u8(p + 1));
            raw.push_back({type, r.u64(p + 8)});
            p += 16;
        }
        // vector data sits here, then the parameter name hashes
        uint64_t hashes_ptr = p;
        for (auto& [type, dp] : raw) hashes_ptr += 16 * type;
        for (uint8_t i = 0; i < count; i++) {
            ShaderParam sp;
            sp.name_hash = r.u32(hashes_ptr + 4 * i);
            sp.slot = slots[i];
            auto [type, dp] = raw[i];
            if (type == 0) {
                sp.type = ParamType::Texture;
                sp.texture = read_texture_ref(r, dp);
            } else {
                sp.type = type == 1 ? ParamType::Vector : ParamType::Array;
                sp.values = r.array<Vec4>(dp, type);
                if (sp.values.empty()) sp.values.resize(type);
            }
            s.params.push_back(std::move(sp));
        }
    }
    return s;
}

ShaderGroup read_shader_group(const Reader& r, uint64_t ptr) {
    ShaderGroup sg;
    uint64_t txd_ptr = r.u64(ptr + 8);
    uint64_t shaders_ptr = r.u64(ptr + 0x10);
    uint16_t count = r.u16(ptr + 0x18);
    if (txd_ptr) sg.textures = read_txd(r, txd_ptr);
    auto ptrs = r.array<uint64_t>(shaders_ptr, count);
    for (auto p : ptrs) sg.shaders.push_back(p ? read_shader(r, p) : Shader{});
    return sg;
}

Skeleton read_skeleton(const Reader& r, uint64_t ptr) {
    Skeleton s;
    auto c = r.cursor(ptr + 0x10);
    c.u64();                             // bone tags pointer (hash table; rebuilt on write)
    c.u16(); c.u16();
    s.unk1C = c.u32();
    uint64_t bones_ptr = c.u64();        // 0x20
    uint64_t inv_ptr = c.u64();          // 0x28
    uint64_t tr_ptr = c.u64();           // 0x30
    uint64_t parents_ptr = c.u64();      // 0x38
    uint64_t children_ptr = c.u64();     // 0x40
    c.u64();
    s.unk50 = c.u32(); s.unk54 = c.u32(); s.unk58 = c.u32();
    c.u16();
    uint16_t bone_count = c.u16();       // 0x5E
    uint16_t child_count = c.u16();      // 0x60
    for (uint16_t i = 0; i < bone_count; i++) {
        uint64_t bp = bones_ptr + (uint64_t)i * 80;
        auto bc = r.cursor(bp);
        Bone b;
        Vec4 q = bc.vec4();
        b.rotation = {q.x, q.y, q.z, q.w};
        b.translation = bc.vec3();
        bc.u32();
        b.scale = bc.vec3();
        bc.f32();
        b.next_sibling = bc.i16();
        b.parent = bc.i16();
        bc.u32();
        uint64_t name_ptr = bc.u64();
        b.flags = bc.u16();
        b.index = bc.i16();
        b.tag = bc.u16();
        b.name = r.str(name_ptr);
        s.bones.push_back(std::move(b));
    }
    s.transforms_inverse = r.array<Mat4>(inv_ptr, bone_count);
    s.transforms = r.array<Mat4>(tr_ptr, bone_count);
    s.parent_indices = r.array<int16_t>(parents_ptr, bone_count);
    s.child_indices = r.array<int16_t>(children_ptr, child_count);
    for (size_t i = 0; i < s.bones.size() && i < s.transforms.size(); i++) {
        auto& m = s.transforms[i];
        s.bones[i].transform_unk = {m.at(0, 3), m.at(1, 3), m.at(2, 3), m.at(3, 3)};
    }
    return s;
}

Joints read_joints(const Reader& r, uint64_t ptr) {
    Joints j;
    uint64_t rot_ptr = r.u64(ptr + 0x10);
    uint64_t tr_ptr = r.u64(ptr + 0x18);
    uint16_t rot_count = r.u16(ptr + 0x30);
    uint16_t tr_count = r.u16(ptr + 0x32);
    j.rotation_limits = r.bytes(rot_ptr, (size_t)rot_count * 0xC0);
    j.translation_limits = r.bytes(tr_ptr, (size_t)tr_count * 0x40);
    return j;
}

Light read_light(const Reader& r, uint64_t ptr) {
    Light l;
    auto c = r.cursor(ptr + 8);
    l.position = c.vec3();
    c.u32();
    l.colour[0] = c.u8(); l.colour[1] = c.u8(); l.colour[2] = c.u8();
    l.flashiness = c.u8();
    l.intensity = c.f32();
    l.flags = c.u32();
    l.bone_id = c.u16();
    l.type = (LightType)c.u8();
    l.group_id = c.u8();
    l.time_flags = c.u32();
    l.falloff = c.f32();
    l.falloff_exponent = c.f32();
    l.culling_plane_normal = c.vec3();
    l.culling_plane_offset = c.f32();
    l.shadow_blur = c.u8();
    l.unk45 = c.u8();
    l.unk46 = c.u16();
    c.u32();
    l.volume_intensity = c.f32();
    l.volume_size_scale = c.f32();
    l.volume_outer_colour[0] = c.u8(); l.volume_outer_colour[1] = c.u8(); l.volume_outer_colour[2] = c.u8();
    l.light_hash = c.u8();
    l.volume_outer_intensity = c.f32();
    l.corona_size = c.f32();
    l.volume_outer_exponent = c.f32();
    l.light_fade = c.u8(); l.shadow_fade = c.u8(); l.specular_fade = c.u8(); l.volumetric_fade = c.u8();
    l.shadow_near_clip = c.f32();
    l.corona_intensity = c.f32();
    l.corona_z_bias = c.f32();
    l.direction = c.vec3();
    l.tangent = c.vec3();
    l.cone_inner = c.f32();
    l.cone_outer = c.f32();
    l.extent = c.vec3();
    l.projected_texture_hash = c.u32();
    return l;
}

} // namespace

Light read_light_at(const Reader& r, uint64_t ptr) { return read_light(r, ptr); }

void read_drawable_base(const Reader& r, uint64_t ptr, Drawable& d) {
    auto c = r.cursor(ptr + 0x10);
    uint64_t sg_ptr = c.u64();
    uint64_t sk_ptr = c.u64();
    d.bounding_center = c.vec3();
    d.bounding_radius = c.f32();
    d.bbox_min = c.vec3();
    d.unk3C = c.u32();
    d.bbox_max = c.vec3();
    d.unk4C = c.u32();
    uint64_t lod_ptr[4];
    for (auto& p : lod_ptr) p = c.u64();
    for (auto& f : d.lod_distance) f = c.f32();
    for (auto& f : d.render_mask_flags) f = c.u32();
    uint64_t joints_ptr = c.u64();       // 0x90
    c.u16(); c.u16(); c.u32();
    uint64_t models_ptr = c.u64();       // 0xA0
    if (sg_ptr) d.shader_group = read_shader_group(r, sg_ptr);
    if (sk_ptr) d.skeleton = read_skeleton(r, sk_ptr);
    if (joints_ptr) d.joints = read_joints(r, joints_ptr);
    for (int i = 0; i < 4; i++) d.lods[i] = read_model_list(r, lod_ptr[i]);
    if (models_ptr && models_ptr != lod_ptr[0]) d.lods_extra = read_model_list(r, models_ptr);
}

Drawable read_drawable(const Reader& r, uint64_t ptr) {
    Drawable d;
    read_drawable_base(r, ptr, d);
    auto c = r.cursor(ptr + 0xA8);
    uint64_t name_ptr = c.u64();
    uint64_t lights_ptr = c.u64();
    uint16_t lights_count = c.u16();
    c.u16(); c.u32();
    c.u64();                             // unknown pointer, always 0
    uint64_t bound_ptr = c.u64();        // 0xC8
    d.name = r.str(name_ptr);
    for (uint16_t i = 0; i < lights_count; i++) d.lights.push_back(read_light(r, lights_ptr + (uint64_t)i * 168));
    if (bound_ptr) d.bound = read_bound(r, bound_ptr);
    return d;
}

Drawable read_ydr(std::span<const uint8_t> file) {
    rsc::Resource res = rsc::Resource::load(file);
    Reader r(res);
    return read_drawable(r, rsc::kSystemBase);
}

DrawableDictionary read_ydd(std::span<const uint8_t> file) {
    rsc::Resource res = rsc::Resource::load(file);
    Reader r(res);
    DrawableDictionary dd;
    uint64_t hashes_ptr = r.u64(rsc::kSystemBase + 0x20);
    uint16_t hashes_count = r.u16(rsc::kSystemBase + 0x28);
    uint64_t draw_ptr = r.u64(rsc::kSystemBase + 0x30);
    uint16_t draw_count = r.u16(rsc::kSystemBase + 0x38);
    dd.hashes = r.array<uint32_t>(hashes_ptr, hashes_count);
    auto ptrs = r.array<uint64_t>(draw_ptr, draw_count);
    for (auto p : ptrs) dd.drawables.push_back(p ? read_drawable(r, p) : Drawable{});
    return dd;
}

// ================================================================ writing

namespace {

constexpr size_t align16(size_t o) { return (o + 15) & ~size_t(15); }

/// Vertex and index data of the game's own files live in the system segment.
constexpr Segment kMeshSegment = Segment::System;

size_t geometry_size(const Geometry& g) {
    size_t n = 152;
    if (!g.bone_ids.empty()) {
        if (g.bone_ids.size() > 4) n += 8;
        n += g.bone_ids.size() * 2;
    }
    return n;
}

size_t model_size(const Model& m) {
    size_t n = m.geometries.size();
    size_t off = 48 + n * 2;
    if (n == 1) off += 6; else off = align16(off);
    off += n * 8;
    off = align16(off);
    off += (n + (n > 1 ? 1 : 0)) * 32;
    for (auto& g : m.geometries) { off = align16(off); off += geometry_size(g); }
    return off;
}

/// Writes a geometry struct at `off` inside block `blk` (its buffers become separate blocks).
void write_geometry(Builder& b, BlockId blk, size_t off, const Geometry& g) {
    uint16_t stride = g.layout.stride();
    if (g.vertices.size() != (size_t)g.vertex_count * stride)
        throw Error("geometry vertex data size does not match vertex_count * stride");
    // vertex buffer
    BlockId vb = b.alloc(Segment::System, 128);
    BlockId vdata = b.array(kMeshSegment, g.vertices);
    BlockId layout = b.alloc(Segment::System, 16);
    {
        BlockWriter w(b, layout);
        w.u32(g.layout.flags);
        w.u16(stride);
        w.u8(0);
        w.u8(g.layout.count());
        w.u64(g.layout.types);
    }
    {
        BlockWriter w(b, vb);
        w.u32(vft::kVertexBuffer);
        w.u32(1);
        w.u16(stride);
        w.u16(g.vb_flags);
        w.u32(0);
        w.ptr(vdata);
        w.u32(g.vertex_count);
        w.u32(0);
        w.ptr(vdata);
        w.u64(0);
        w.ptr(layout);
    }
    // index buffer
    BlockId ib = b.alloc(Segment::System, 96);
    BlockId idata = b.array(kMeshSegment, g.indices);
    {
        BlockWriter w(b, ib);
        w.u32(vft::kIndexBuffer);
        w.u32(1);
        w.u32((uint32_t)g.indices.size());
        w.u32(0);
        w.ptr(idata);
    }
    BlockWriter w(b, blk, off);
    w.u32(vft::kGeometry);
    w.u32(1);
    w.zero(16);
    w.ptr(vb);
    w.zero(24);
    w.ptr(ib);
    w.zero(24);
    w.u32((uint32_t)g.indices.size());
    w.u32((uint32_t)(g.indices.size() / 3));
    w.u16((uint16_t)g.vertex_count);
    w.u16(3);
    w.u32(0);
    size_t ids_off = off + 152 + (g.bone_ids.size() > 4 ? 8 : 0);
    if (!g.bone_ids.empty()) w.ptr(blk, ids_off); else w.u64(0);
    w.u16(stride);
    w.u16((uint16_t)g.bone_ids.size());
    w.u32(0);
    w.ptr(vdata);
    w.zero(24);
    if (!g.bone_ids.empty()) {
        w.seek(ids_off);
        for (auto id : g.bone_ids) w.u16(id);
    }
}

/// Writes a model struct (with its geometries embedded) at `off` inside `blk`.
void write_model(Builder& b, BlockId blk, size_t off, const Model& m) {
    size_t n = m.geometries.size();
    size_t mapping_off = off + 48;
    size_t o = mapping_off + n * 2;
    if (n == 1) o += 6; else o = align16(o);
    size_t ptrs_off = o;
    o += n * 8;
    o = align16(o);
    size_t bounds_off = o;
    o += (n + (n > 1 ? 1 : 0)) * 32;
    std::vector<size_t> geom_off;
    for (auto& g : m.geometries) { o = align16(o); geom_off.push_back(o); o += geometry_size(g); }

    BlockWriter w(b, blk, off);
    w.u32(vft::kModel);
    w.u32(1);
    w.ptr(blk, ptrs_off);
    w.u16((uint16_t)n);
    w.u16((uint16_t)n);
    w.u32(0);
    w.ptr(blk, bounds_off);
    w.ptr(blk, mapping_off);
    w.u32(m.skeleton_binding);
    w.u16((uint16_t)(m.render_mask | ((uint16_t)m.flags << 8)));
    w.u16((uint16_t)n);
    w.seek(mapping_off);
    for (auto& g : m.geometries) w.u16(g.shader_index);
    w.seek(ptrs_off);
    for (size_t i = 0; i < n; i++) w.ptr(blk, geom_off[i]);
    w.seek(bounds_off);
    if (n > 1) { w.vec4(m.bounds.min); w.vec4(m.bounds.max); }
    for (auto& g : m.geometries) { w.vec4(g.bounds.min); w.vec4(g.bounds.max); }
    for (size_t i = 0; i < n; i++) write_geometry(b, blk, geom_off[i], m.geometries[i]);
}

BlockId write_shader(Builder& b, const Shader& s, const TextureBlocks& textures) {
    size_t n = s.params.size();
    size_t base = 32;
    uint16_t param_size = 0;
    for (auto& p : s.params) {
        uint8_t type = p.type == ParamType::Texture ? 0 : (uint8_t)std::max<size_t>(1, p.values.size());
        base += 16 + 16 * type;
        param_size = (uint16_t)(param_size + 16 + 16 * type);
    }
    base += n * 4;
    uint16_t data_size = (uint16_t)align16(base);
    BlockId params = n ? b.alloc(Segment::System, base + (size_t)data_size * 4) : BlockId{};
    if (n) {
        // register slots the game expects when none were read
        std::vector<uint8_t> slots(n);
        int vslot = 160;
        for (size_t i = 0; i < n; i++) slots[i] = s.params[i].slot < 0 ? 0 : (uint8_t)s.params[i].slot;
        for (size_t i = 0; i < n; i++)
            if (s.params[i].type == ParamType::Texture && s.params[i].slot < 0) slots[i] = (uint8_t)(i + 2);
        for (size_t i = n; i-- > 0;) {
            if (s.params[i].type != ParamType::Texture) {
                if (s.params[i].slot < 0) slots[i] = (uint8_t)vslot;
                vslot += (int)std::max<size_t>(1, s.params[i].values.size());
            }
        }
        BlockWriter w(b, params);
        size_t data_off = n * 16;
        for (size_t i = 0; i < n; i++) {
            auto& p = s.params[i];
            if (p.type == ParamType::Texture) {
                w.u8(0);
                w.u8(slots[i]);
                w.u16(0);
                w.u32(0);
                BlockId tb;
                if (!p.texture.empty()) {
                    auto it = textures.find(joaat_lower(p.texture));
                    tb = it != textures.end() ? it->second : write_texture_ref(b, p.texture);
                }
                w.ptr(tb);
            } else {
                size_t count = std::max<size_t>(1, p.values.size());
                w.u8((uint8_t)count);
                w.u8(slots[i]);
                w.u16(0);
                w.u32(0);
                w.ptr(params, data_off);
                size_t here = w.pos();
                w.seek(data_off);
                for (size_t k = 0; k < count; k++) w.vec4(k < p.values.size() ? p.values[k] : Vec4{});
                data_off = w.pos();
                w.seek(here);
            }
        }
        w.seek(data_off);
        for (auto& p : s.params) w.u32(p.name_hash);
    }
    uint8_t tex_count = 0;
    for (auto& p : s.params) if (p.type == ParamType::Texture) tex_count++;
    BlockId blk = b.alloc(Segment::System, 48);
    BlockWriter w(b, blk);
    w.ptr(params);
    w.u32(s.name_hash);
    w.u32(0);
    w.u8((uint8_t)n);
    w.u8(s.render_bucket);
    w.u16(s.unk12);
    w.u16(param_size);
    w.u16(data_size);
    w.u32(s.sps_hash);
    w.u32(0);
    w.u32(s.render_bucket_mask());
    w.u16(0);
    w.u8(0);
    w.u8(tex_count);
    w.u64(0);
    return blk;
}

BlockId write_shader_group(Builder& b, const ShaderGroup& sg) {
    BlockId blk = b.alloc(Segment::System, 64);
    TextureBlocks textures;
    BlockId txd;
    if (sg.textures) txd = write_txd(b, *sg.textures, &textures, false);
    BlockId ptrs;
    if (!sg.shaders.empty()) {
        ptrs = b.alloc(Segment::System, sg.shaders.size() * 8);
        for (size_t i = 0; i < sg.shaders.size(); i++)
            b.pointer(ptrs, i * 8, write_shader(b, sg.shaders[i], textures));
    }
    BlockWriter w(b, blk);
    w.u32(vft::kShaderGroup);
    w.u32(1);
    w.ptr(txd);
    w.ptr(ptrs);
    w.u16((uint16_t)sg.shaders.size());
    w.u16((uint16_t)sg.shaders.size());
    w.u32(0);
    w.u64(0);
    w.u64(0);
    w.u32(64 / 16);
    w.u32(0);
    w.u64(0);
    return blk;
}

uint32_t hash_buckets(size_t n) {
    static const uint32_t sizes[] = {11, 29, 59, 107, 191, 331, 563, 953, 1609, 2729, 4621, 7841, 13297, 22571, 38351, 65167};
    for (auto s : sizes) if (n < s) return s;
    return 65521;
}

BlockId write_skeleton(Builder& b, const Skeleton& sk) {
    size_t n = sk.bones.size();
    // bones: 16 byte header then 80 bytes per bone
    BlockId bones = b.alloc(Segment::System, 16 + n * 80);
    {
        BlockWriter w(b, bones);
        w.u32((uint32_t)n);
        w.zero(12);
        for (auto& bone : sk.bones) {
            BlockId name = bone.name.empty() ? BlockId{} : b.string(bone.name);
            w.vec4({bone.rotation.x, bone.rotation.y, bone.rotation.z, bone.rotation.w});
            w.vec3(bone.translation);
            w.u32(0);
            w.vec3(bone.scale);
            w.f32(1.0f);
            w.i16(bone.next_sibling);
            w.i16(bone.parent);
            w.u32(0);
            w.ptr(name);
            w.u16(bone.flags);
            w.i16(bone.index);
            w.u16(bone.tag);
            w.i16(bone.index);
            w.u64(0);
        }
    }
    // transforms
    std::vector<Mat4> tr = sk.transforms, inv = sk.transforms_inverse;
    if (tr.size() != n || inv.size() != n) {
        tr.clear(); inv.clear();
        for (size_t i = 0; i < n; i++) {
            auto& bone = sk.bones[i];
            Mat4 m = mat_from_quat_pos(bone.rotation, bone.translation);
            m.at(0, 0) *= bone.scale.x; m.at(1, 1) *= bone.scale.y; m.at(2, 2) *= bone.scale.z;
            m.at(0, 3) = bone.transform_unk.x; m.at(1, 3) = bone.transform_unk.y;
            m.at(2, 3) = bone.transform_unk.z; m.at(3, 3) = bone.transform_unk.w;
            Mat4 mi = affine_inverse(sk.bind_matrix(i));
            mi.at(0, 3) = mi.at(1, 3) = mi.at(2, 3) = mi.at(3, 3) = 0;
            tr.push_back(m);
            inv.push_back(mi);
        }
    }
    std::vector<int16_t> parents = sk.parent_indices, children = sk.child_indices;
    if (parents.size() != n) {
        parents.clear();
        for (auto& bone : sk.bones) parents.push_back(bone.parent);
    }
    if (children.empty() && n > 0) {
        // breadth-wise walk limited to 4 per layer, padded to 8 entries (CodeWalker's reconstruction)
        auto kids_of = [&](int16_t idx) {
            std::vector<int16_t> r;
            for (auto& bone : sk.bones) if (bone.parent == idx) r.push_back(bone.index);
            return r;
        };
        std::vector<int16_t> layer;
        for (auto& bone : sk.bones) if (bone.parent < 0) for (auto k : kids_of(bone.index)) layer.push_back(k);
        while (!layer.empty()) {
            size_t take = std::min<size_t>(layer.size(), 4);
            std::vector<int16_t> ins(layer.begin(), layer.begin() + (ptrdiff_t)take);
            std::vector<int16_t> ext;
            for (auto i : ins) for (auto k : kids_of(i)) ext.push_back(k);
            layer.erase(layer.begin(), layer.begin() + (ptrdiff_t)take);
            layer.insert(layer.begin(), ext.begin(), ext.end());
            int16_t last = -1, last_parent = -1;
            for (auto i : ins) {
                children.push_back(i);
                children.push_back(sk.bones[i].parent);
                last = i; last_parent = sk.bones[i].parent;
            }
            size_t pad = 8 - (children.size() % 8);
            if (pad < 8) for (size_t k = 0; k < pad; k += 2) { children.push_back(last); children.push_back(last_parent); }
        }
    }
    BlockId tr_blk = b.array(Segment::System, tr);
    BlockId inv_blk = b.array(Segment::System, inv);
    BlockId par_blk = b.array(Segment::System, parents);
    BlockId chi_blk = b.array(Segment::System, children);

    // bone tag hash table: buckets of (tag, index) chained through pointers
    BlockId tags;
    uint32_t buckets = 0;
    if (n >= 2) {
        buckets = hash_buckets(n);
        std::vector<std::vector<size_t>> bucket(buckets);
        for (size_t i = 0; i < n; i++) bucket[sk.bones[i].tag % buckets].push_back(i);
        tags = b.alloc(Segment::System, (size_t)buckets * 8);
        for (uint32_t bi = 0; bi < buckets; bi++) {
            auto& list = bucket[bi];
            if (list.empty()) continue;
            std::reverse(list.begin(), list.end());
            BlockId prev;
            for (size_t k = 0; k < list.size(); k++) {
                BlockId e = b.alloc(Segment::System, 16);
                BlockWriter w(b, e);
                w.u32(sk.bones[list[k]].tag);
                w.u32((uint32_t)list[k]);
                if (k == 0) b.pointer(tags, (size_t)bi * 8, e);
                else b.pointer(prev, 8, e);
                prev = e;
            }
        }
    }

    BlockId blk = b.alloc(Segment::System, 112);
    BlockWriter w(b, blk);
    w.u32(vft::kSkeleton);
    w.u32(1);
    w.u64(0);
    w.ptr(tags);
    w.u16((uint16_t)buckets);
    w.u16((uint16_t)std::min<size_t>(n, buckets));
    w.u32(sk.unk1C);
    w.ptr(bones, 16);
    w.ptr(inv_blk);
    w.ptr(tr_blk);
    w.ptr(par_blk);
    w.ptr(chi_blk);
    w.u64(0);
    w.u32(sk.unk50); w.u32(sk.unk54); w.u32(sk.unk58);
    w.u16(1);
    w.u16((uint16_t)n);
    w.u16((uint16_t)children.size());
    w.u16(0);
    w.u32(0);
    w.u64(0);
    return blk;
}

BlockId write_joints(Builder& b, const Joints& j) {
    BlockId blk = b.alloc(Segment::System, 64);
    BlockId rot = b.array(Segment::System, j.rotation_limits);
    BlockId tr = b.array(Segment::System, j.translation_limits);
    BlockWriter w(b, blk);
    w.u32(vft::kJoints);
    w.u32(1);
    w.u64(0);
    w.ptr(rot);
    w.ptr(tr);
    w.u64(0);
    w.u64(0);
    w.u16((uint16_t)(j.rotation_limits.size() / 0xC0));
    w.u16((uint16_t)(j.translation_limits.size() / 0x40));
    w.u16(0);
    w.u16(1);
    w.u64(0);
    return blk;
}

void write_light(BlockWriter& w, const Light& l) {
    w.u64(0);
    w.vec3(l.position);
    w.u32(0);
    w.u8(l.colour[0]); w.u8(l.colour[1]); w.u8(l.colour[2]);
    w.u8(l.flashiness);
    w.f32(l.intensity);
    w.u32(l.flags);
    w.u16(l.bone_id);
    w.u8((uint8_t)l.type);
    w.u8(l.group_id);
    w.u32(l.time_flags);
    w.f32(l.falloff);
    w.f32(l.falloff_exponent);
    w.vec3(l.culling_plane_normal);
    w.f32(l.culling_plane_offset);
    w.u8(l.shadow_blur);
    w.u8(l.unk45);
    w.u16(l.unk46);
    w.u32(0);
    w.f32(l.volume_intensity);
    w.f32(l.volume_size_scale);
    w.u8(l.volume_outer_colour[0]); w.u8(l.volume_outer_colour[1]); w.u8(l.volume_outer_colour[2]);
    w.u8(l.light_hash);
    w.f32(l.volume_outer_intensity);
    w.f32(l.corona_size);
    w.f32(l.volume_outer_exponent);
    w.u8(l.light_fade); w.u8(l.shadow_fade); w.u8(l.specular_fade); w.u8(l.volumetric_fade);
    w.f32(l.shadow_near_clip);
    w.f32(l.corona_intensity);
    w.f32(l.corona_z_bias);
    w.vec3(l.direction);
    w.vec3(l.tangent);
    w.f32(l.cone_inner);
    w.f32(l.cone_outer);
    w.vec3(l.extent);
    w.u32(l.projected_texture_hash);
    w.u32(0);
}

} // namespace

void write_light_at(BlockWriter& w, const Light& l) { write_light(w, l); }

DrawableBlocks prepare_drawable(Builder& b, const Drawable& d) {
    DrawableBlocks parts;
    if (d.shader_group) parts.shader_group = write_shader_group(b, *d.shader_group);
    if (d.skeleton) parts.skeleton = write_skeleton(b, *d.skeleton);
    if (d.joints) parts.joints = write_joints(b, *d.joints);

    // one block holding every LOD list: header + pointers, then the models (geometries embedded)
    const std::vector<Model>* lists[5] = {&d.lods[0], &d.lods[1], &d.lods[2], &d.lods[3], &d.lods_extra};
    size_t off = 0;
    std::vector<std::vector<size_t>> model_off(5);
    for (int l = 0; l < 5; l++) {
        if (lists[l]->empty()) { parts.models_offsets[l] = UINT64_MAX; continue; }
        off = align16(off);
        parts.models_offsets[l] = off;
        off += 16 + lists[l]->size() * 8;
        for (auto& m : *lists[l]) { off = align16(off); model_off[l].push_back(off); off += model_size(m); }
    }
    parts.models_size = off;
    if (off > 0) {
        parts.models = b.alloc(Segment::System, off);
        for (int l = 0; l < 5; l++) {
            if (lists[l]->empty()) continue;
            size_t lo = (size_t)parts.models_offsets[l];
            BlockWriter w(b, parts.models, lo);
            w.ptr(parts.models, lo + 16);
            w.u16((uint16_t)lists[l]->size());
            w.u16((uint16_t)lists[l]->size());
            w.u32(0);
            for (size_t i = 0; i < lists[l]->size(); i++) w.ptr(parts.models, model_off[l][i]);
            for (size_t i = 0; i < lists[l]->size(); i++) write_model(b, parts.models, model_off[l][i], (*lists[l])[i]);
        }
    }
    return parts;
}

void write_drawable_base(BlockWriter& w, const Drawable& d, const DrawableBlocks& parts, Builder& b) {
    w.ptr(parts.shader_group);
    w.ptr(parts.skeleton);
    w.vec3(d.bounding_center);
    w.f32(d.bounding_radius);
    w.vec3(d.bbox_min);
    w.u32(d.unk3C);
    w.vec3(d.bbox_max);
    w.u32(d.unk4C);
    for (int l = 0; l < 4; l++) {
        if (parts.models_offsets[l] == UINT64_MAX) w.u64(0);
        else w.ptr(parts.models, parts.models_offsets[l]);
    }
    for (auto f : d.lod_distance) w.f32(f);
    for (auto f : d.render_mask_flags) w.u32(f);
    w.ptr(parts.joints);
    w.u16(0);
    w.u16((uint16_t)((parts.models_size + 15) / 16));
    w.u32(0);
    w.ptr(parts.models);
}

BlockId write_drawable(Builder& b, const Drawable& d, bool root) {
    BlockId blk = b.alloc(Segment::System, 208);
    BlockId pages = root ? b.alloc_pages_info() : BlockId{};
    DrawableBlocks parts = prepare_drawable(b, d);
    BlockId name = d.name.empty() ? BlockId{} : b.string(d.name);
    BlockId lights;
    if (!d.lights.empty()) {
        lights = b.alloc(Segment::System, d.lights.size() * 168);
        BlockWriter lw(b, lights);
        for (auto& l : d.lights) write_light(lw, l);
    }
    BlockId bound = d.bound ? write_bound(b, *d.bound) : BlockId{};
    BlockWriter w(b, blk);
    w.u32(vft::kDrawable);
    w.u32(1);
    w.ptr(pages);
    write_drawable_base(w, d, parts, b);
    w.ptr(name);
    w.ptr(lights);
    w.u16((uint16_t)d.lights.size());
    w.u16((uint16_t)d.lights.size());
    w.u32(0);
    w.u64(0);
    w.ptr(bound);
    return blk;
}

Bytes write_ydr(const Drawable& d) {
    Builder b;
    write_drawable(b, d, true);
    return b.build(rsc::kYdr);
}

Bytes write_ydd(const DrawableDictionary& dd) {
    if (dd.hashes.size() != dd.drawables.size()) throw Error("drawable dictionary: hashes and drawables differ in count");
    // the game keeps dictionaries sorted by hash
    std::vector<size_t> order(dd.drawables.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t c) { return dd.hashes[a] < dd.hashes[c]; });

    Builder b;
    BlockId blk = b.alloc(Segment::System, 64);
    BlockId pages = b.alloc_pages_info();
    std::vector<uint32_t> hashes;
    BlockId ptrs = dd.drawables.empty() ? BlockId{} : b.alloc(Segment::System, dd.drawables.size() * 8);
    for (size_t i = 0; i < order.size(); i++) {
        hashes.push_back(dd.hashes[order[i]]);
        b.pointer(ptrs, i * 8, write_drawable(b, dd.drawables[order[i]], false));
    }
    BlockId hashes_blk = b.array(Segment::System, hashes);
    BlockWriter w(b, blk);
    w.u32(vft::kDrawableDict);
    w.u32(1);
    w.ptr(pages);
    w.u64(0);
    w.u64(1);
    w.ptr(hashes_blk);
    w.u16((uint16_t)hashes.size());
    w.u16((uint16_t)hashes.size());
    w.u32(0);
    w.ptr(ptrs);
    w.u16((uint16_t)hashes.size());
    w.u16((uint16_t)hashes.size());
    w.u32(0);
    return b.build(rsc::kYdd);
}

namespace {
float tan_dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 tan_cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 tan_norm(const Vec3& v) { const float l = v.length(); return l > 1e-12f ? v * (1.f / l) : Vec3{0, 0, 0}; }
}   // namespace

void compute_tangents(Geometry& g, size_t first_vertex) {
    if (!g.layout.has(VertexSemantic::Tangent) || !g.layout.has(VertexSemantic::Position) || !g.layout.has(VertexSemantic::Normal) ||
        !g.layout.has(VertexSemantic::TexCoord0) || first_vertex >= g.vertex_count)
        return;
    const size_t n = g.vertex_count;
    std::vector<Vec3> tu(n), tv(n);
    for (size_t k = 0; k + 2 < g.indices.size(); k += 3) {
        const uint32_t a = g.indices[k], b = g.indices[k + 1], c = g.indices[k + 2];
        if (a < first_vertex && b < first_vertex && c < first_vertex) continue;
        if (a >= n || b >= n || c >= n) continue;
        const Vec3 pa = g.position(a), e1 = g.position(b) - pa, e2 = g.position(c) - pa;
        const Vec2 ua = g.uv(a), d1{g.uv(b).x - ua.x, g.uv(b).y - ua.y}, d2{g.uv(c).x - ua.x, g.uv(c).y - ua.y};
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f) continue;
        const float r = 1.f / det;
        const Vec3 dpdu = (e1 * d2.y - e2 * d1.y) * r, dpdv = (e2 * d1.x - e1 * d2.x) * r;
        for (uint32_t i : {a, b, c}) { tu[i] = tu[i] + dpdu; tv[i] = tv[i] + dpdv; }
    }
    for (size_t i = first_vertex; i < n; i++) {
        const Vec3 nn = g.normal((uint32_t)i);
        Vec3 t = tu[i] - nn * tan_dot(nn, tu[i]);
        if (tan_dot(t, t) < 1e-12f) t = std::fabs(nn.z) > 0.9f ? Vec3{1, 0, 0} : tan_norm(tan_cross(Vec3{0, 0, 1}, nn));   // a degenerate mapping: any tangent across the normal
        t = tan_norm(t);
        const float w = tan_dot(tan_cross(nn, t), tv[i]) > 0 ? -1.f : 1.f;
        g.set((uint32_t)i, VertexSemantic::Tangent, Vec4(t, w));
        if (g.layout.has(VertexSemantic::Binormal)) g.set((uint32_t)i, VertexSemantic::Binormal, Vec4(tan_cross(nn, t) * w, 0));
    }
}

} // namespace rk
