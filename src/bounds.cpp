#include "ragekit/bounds.h"
#include "ragekit/error.h"
#include "bounds_io.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace rk {

const char* bound_type_name(BoundType t) {
    switch (t) {
        case BoundType::Sphere: return "Sphere";
        case BoundType::Capsule: return "Capsule";
        case BoundType::Box: return "Box";
        case BoundType::Geometry: return "Geometry";
        case BoundType::GeometryBVH: return "GeometryBVH";
        case BoundType::Composite: return "Composite";
        case BoundType::Disc: return "Disc";
        case BoundType::Cylinder: return "Cylinder";
        case BoundType::Cloth: return "Cloth";
        default: return "Unknown";
    }
}

// ================================================================ polygons

namespace {
template <class T> T rd(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }
template <class T> void wr(uint8_t* p, T v) { std::memcpy(p, &v, sizeof(T)); }
}

float BoundPolygon::area() const { return kind == Triangle ? rd<float>(raw.data()) : 0; }
void BoundPolygon::set_area(float a) { if (kind == Triangle) wr(raw.data(), a); }

uint16_t BoundPolygon::vertex(int i) const {
    switch (kind) {
        case Triangle: return (uint16_t)(rd<uint16_t>(raw.data() + 4 + 2 * i) & 0x7FFF);
        case Sphere: return rd<uint16_t>(raw.data() + 2);
        case Capsule: case Cylinder: return i == 0 ? rd<uint16_t>(raw.data() + 2) : rd<uint16_t>(raw.data() + 8);
        case Box: return (uint16_t)rd<int16_t>(raw.data() + 4 + 2 * i);
    }
    return 0;
}
bool BoundPolygon::vertex_flag(int i) const { return kind == Triangle && (rd<uint16_t>(raw.data() + 4 + 2 * i) & 0x8000); }
void BoundPolygon::set_vertex(int i, uint16_t v) {
    switch (kind) {
        case Triangle: { uint8_t* q = raw.data() + 4 + 2 * i; wr(q, (uint16_t)((rd<uint16_t>(q) & 0x8000) | (v & 0x7FFF))); break; }
        case Sphere: wr(raw.data() + 2, v); break;
        case Capsule: case Cylinder: wr(raw.data() + (i == 0 ? 2 : 8), v); break;
        case Box: wr(raw.data() + 4 + 2 * i, (int16_t)v); break;
    }
}
uint16_t BoundPolygon::edge(int i) const { return kind == Triangle ? rd<uint16_t>(raw.data() + 10 + 2 * i) : 0xFFFF; }
void BoundPolygon::set_edge(int i, int32_t poly) {
    if (kind != Triangle) return;
    wr(raw.data() + 10 + 2 * i, (uint16_t)(poly < 0 || poly > 0xFFFF ? 0xFFFF : poly));
}
float BoundPolygon::radius() const { return (kind == Sphere || kind == Capsule || kind == Cylinder) ? rd<float>(raw.data() + 4) : 0; }
void BoundPolygon::set_radius(float r) { if (kind == Sphere || kind == Capsule || kind == Cylinder) wr(raw.data() + 4, r); }

BoundPolygon BoundPolygon::triangle(uint16_t a, uint16_t b, uint16_t c) {
    BoundPolygon p; p.kind = Triangle;
    wr(p.raw.data() + 4, a); wr(p.raw.data() + 6, b); wr(p.raw.data() + 8, c);
    wr(p.raw.data() + 10, (uint16_t)0xFFFF); wr(p.raw.data() + 12, (uint16_t)0xFFFF); wr(p.raw.data() + 14, (uint16_t)0xFFFF);
    return p;
}
BoundPolygon BoundPolygon::sphere(uint16_t center, float radius) {
    BoundPolygon p; p.kind = Sphere;
    wr(p.raw.data() + 2, center); wr(p.raw.data() + 4, radius);
    return p;
}
BoundPolygon BoundPolygon::capsule(uint16_t a, uint16_t b, float radius) {
    BoundPolygon p; p.kind = Capsule;
    wr(p.raw.data() + 2, a); wr(p.raw.data() + 4, radius); wr(p.raw.data() + 8, b);
    return p;
}
BoundPolygon BoundPolygon::box(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    BoundPolygon p; p.kind = Box;
    wr(p.raw.data() + 4, (int16_t)a); wr(p.raw.data() + 6, (int16_t)b); wr(p.raw.data() + 8, (int16_t)c); wr(p.raw.data() + 10, (int16_t)d);
    return p;
}
BoundPolygon BoundPolygon::cylinder(uint16_t a, uint16_t b, float radius) {
    BoundPolygon p; p.kind = Cylinder;
    wr(p.raw.data() + 2, a); wr(p.raw.data() + 4, radius); wr(p.raw.data() + 8, b);
    return p;
}

// ================================================================ transforms

Mat4 BoundTransform::matrix() const {
    Mat4 m;
    for (int c = 0; c < 4; c++) { m.at(c, 0) = col[c].x; m.at(c, 1) = col[c].y; m.at(c, 2) = col[c].z; m.at(c, 3) = c == 3 ? 1.f : 0.f; }
    return m;
}
BoundTransform BoundTransform::from(const Mat4& m) {
    BoundTransform t;
    for (int c = 0; c < 4; c++) t.col[c] = {m.at(c, 0), m.at(c, 1), m.at(c, 2)};
    return t;
}

// xml: the file layout is column xyz + flag word, four times (see bounds.h). Until 2026-10-10 the struct was
// copied as it is in memory (12 floats, then 4 flag words), which read game files with scrambled columns and
// wrote new composites the game reads as a garbage matrix.
BoundTransform BoundTransform::from_file(const uint8_t* p) {
    BoundTransform t;
    for (int c = 0; c < 4; c++) {
        float v[3]; std::memcpy(v, p + c * 16, 12);
        std::memcpy(&t.flags[c], p + c * 16 + 12, 4);
        t.col[c] = {v[0], v[1], v[2]};
    }
    return t;
}
void BoundTransform::to_file(uint8_t* p) const {
    for (int c = 0; c < 4; c++) {
        float v[3] = {col[c].x, col[c].y, col[c].z};
        std::memcpy(p + c * 16, v, 12);
        std::memcpy(p + c * 16 + 12, &flags[c], 4);
    }
}

Vec3 Bound::vertex_position(size_t i) const {
    if (!geometry || i >= geometry->vertices.size()) return {};
    return geometry->vertices[i] + geometry->center;
}

// ================================================================ reading

namespace {

BoundBVH read_bvh(const Reader& r, uint64_t ptr) {
    BoundBVH b;
    auto c = r.cursor(ptr);
    uint64_t nodes_ptr = c.u64();
    b.node_count = c.u32();
    uint32_t cap = c.u32();
    c.skip(16);
    b.bbox_min = c.vec4(); b.bbox_max = c.vec4(); b.bbox_center = c.vec4();
    b.quantum_inverse = c.vec4(); b.quantum = c.vec4();
    uint64_t trees_ptr = c.u64();
    uint16_t trees_count = c.u16();
    b.nodes = r.array<BoundBVH::Node>(nodes_ptr, cap);
    b.trees = r.array<BoundBVH::Tree>(trees_ptr, trees_count);
    return b;
}

void read_geometry(const Reader& r, uint64_t ptr, Bound& b) {
    Bound::Geometry g;
    auto c = r.cursor(ptr + 0x70);
    c.u64();
    uint64_t shrunk_ptr = c.u64();        // 0x78
    g.unk80 = c.u16(); g.unk82 = c.u16();
    uint32_t shrunk_count = c.u32();      // 0x84
    uint64_t polys_ptr = c.u64();         // 0x88
    g.quantum = c.vec3(); g.unk9C = c.f32();
    g.center = c.vec3(); g.unkAC = c.f32();
    uint64_t verts_ptr = c.u64();         // 0xB0
    uint64_t colours_ptr = c.u64();       // 0xB8
    uint64_t octants_ptr = c.u64();       // 0xC0
    uint64_t octant_items_ptr = c.u64();  // 0xC8
    uint32_t verts_count = c.u32();       // 0xD0
    uint32_t polys_count = c.u32();       // 0xD4
    c.skip(24);
    uint64_t mats_ptr = c.u64();          // 0xF0
    uint64_t matcol_ptr = c.u64();        // 0xF8
    c.skip(24);
    uint64_t polymat_ptr = c.u64();       // 0x118
    uint8_t mats_count = c.u8();          // 0x120
    uint8_t matcol_count = c.u8();
    auto dequant = [&](const std::vector<int16_t>& q) {
        std::vector<Vec3> v(q.size() / 3);
        for (size_t i = 0; i < v.size(); i++) v[i] = {q[i * 3] * g.quantum.x, q[i * 3 + 1] * g.quantum.y, q[i * 3 + 2] * g.quantum.z};
        return v;
    };
    g.vertices_shrunk = dequant(r.array<int16_t>(shrunk_ptr, (size_t)shrunk_count * 3));
    g.vertices = dequant(r.array<int16_t>(verts_ptr, (size_t)verts_count * 3));
    Bytes polys = r.bytes(polys_ptr, (size_t)polys_count * 16);
    for (uint32_t i = 0; i < polys_count; i++) {
        BoundPolygon p;
        std::memcpy(p.raw.data(), polys.data() + i * 16, 16);
        p.kind = (BoundPolygon::Kind)(p.raw[0] & 7);
        p.raw[0] &= 0xF8;
        g.polygons.push_back(p);
    }
    g.vertex_colours = r.array<uint32_t>(colours_ptr, verts_count);
    if (octants_ptr) {
        g.has_octants = true;
        auto counts = r.array<uint32_t>(octants_ptr, 8);
        auto ptrs = r.array<uint64_t>(octant_items_ptr, 8);
        if (ptrs.size() != 8) throw Error("bound: octant counts without their item lists");
        for (int i = 0; i < 8; i++) g.octants[i] = r.array<uint32_t>(ptrs[i], counts[i]);
    }
    g.materials = r.array<BoundMaterial>(mats_ptr, mats_count);
    g.material_colours = r.array<uint32_t>(matcol_ptr, matcol_count);
    g.polygon_materials = r.array<uint8_t>(polymat_ptr, polys_count);
    if (b.type == BoundType::GeometryBVH) {
        uint64_t bvh_ptr = r.u64(ptr + 0x130);
        g.unk140 = r.u16(ptr + 0x140);
        if (bvh_ptr > 65535) g.bvh = read_bvh(r, bvh_ptr);
    }
    b.geometry = std::move(g);
}

void read_composite(const Reader& r, uint64_t ptr, Bound& b, std::map<uint64_t, std::shared_ptr<Bound>>* seen) {
    auto c = r.cursor(ptr + 0x70);
    uint64_t children_ptr = c.u64();
    uint64_t tr1_ptr = c.u64();
    uint64_t tr2_ptr = c.u64();
    uint64_t boxes_ptr = c.u64();
    uint64_t flags1_ptr = c.u64();
    uint64_t flags2_ptr = c.u64();
    uint16_t count = c.u16();
    c.u16(); c.u32();
    uint64_t bvh_ptr = c.u64();
    auto ptrs = r.array<uint64_t>(children_ptr, count);
    auto read_transforms = [&](uint64_t ptr) { // xml: see BoundTransform::from_file
        std::vector<BoundTransform> out;
        for (auto& raw : r.array<std::array<uint8_t, 64>>(ptr, count)) out.push_back(BoundTransform::from_file(raw.data()));
        return out;
    };
    auto tr1 = read_transforms(tr1_ptr);
    auto tr2 = read_transforms(tr2_ptr);
    auto boxes = r.array<AABB>(boxes_ptr, count);
    auto f1 = r.array<uint32_t>(flags1_ptr, (size_t)count * 2);
    auto f2 = r.array<uint32_t>(flags2_ptr, (size_t)count * 2);
    b.has_child_flags = flags1_ptr != 0 || flags2_ptr != 0;
    b.has_child_transforms2 = tr2_ptr != 0 && tr2_ptr != tr1_ptr;
    if (b.has_child_transforms2) b.child_transforms2 = tr2;
    if (count && ptrs.empty()) throw Error("bound: a composite of " + std::to_string(count) + " children without a child list");
    for (uint16_t i = 0; i < count; i++) {
        BoundChild ch;
        ch.bound = read_bound(r, ptrs[i], seen);
        if (i < tr1.size()) ch.transform = tr1[i];
        else if (i < tr2.size()) ch.transform = tr2[i];
        if (i < boxes.size()) ch.bbox = boxes[i];
        if (f1.size() == (size_t)count * 2) { ch.flags1 = f1[i * 2]; ch.flags2 = f1[i * 2 + 1]; }
        b.children.push_back(std::move(ch));
    }
    if (bvh_ptr) b.composite_bvh = read_bvh(r, bvh_ptr);
}

} // namespace

namespace {
// Composites nest a few levels deep in the game's files. A composite that contains itself is malformed: read with `seen`
// (shared bounds kept shared) it would become a cycle every later walk recurses round without end, so the bounds being
// read are kept on a path and a pointer back into it throws; a chain of a thousand composites stops at the depth limit.
constexpr size_t kMaxBoundDepth = 64;
thread_local std::vector<uint64_t> t_bound_path;
struct BoundPathGuard {
    explicit BoundPathGuard(uint64_t ptr) {
        if (t_bound_path.size() >= kMaxBoundDepth) throw Error("bound: composites nested more than " + std::to_string(kMaxBoundDepth) + " deep");
        t_bound_path.push_back(ptr);
    }
    ~BoundPathGuard() { t_bound_path.pop_back(); }
};
} // namespace

std::shared_ptr<Bound> read_bound(const Reader& r, uint64_t ptr, std::map<uint64_t, std::shared_ptr<Bound>>* seen) {
    if (ptr == 0) return nullptr;
    if (std::find(t_bound_path.begin(), t_bound_path.end(), ptr) != t_bound_path.end()) throw Error("bound: a composite contains itself");
    if (seen) {
        auto it = seen->find(ptr);
        if (it != seen->end() && it->second) return it->second;
    }
    BoundPathGuard path(ptr);
    auto b = std::make_shared<Bound>();
    if (seen) (*seen)[ptr] = b;   // before the children: a child pointing back is not read again
    auto c = r.cursor(ptr + 0x10);
    b->type = (BoundType)c.u8();
    c.u8(); c.u16();
    b->sphere_radius = c.f32();
    c.u32(); c.u32();
    b->box_max = c.vec3();
    b->margin = c.f32();
    b->box_min = c.vec3();
    b->unk3C = c.u32();
    b->box_center = c.vec3();
    b->material_index = c.u8(); b->procedural_id = c.u8(); b->room_ped_density = c.u8(); b->unk_flags = c.u8();
    b->sphere_center = c.vec3();
    b->poly_flags = c.u8(); b->material_colour_index = c.u8();
    b->unk5E = c.u16();
    b->inertia = c.vec3();
    b->volume = c.f32();
    switch (b->type) {
        case BoundType::Geometry: case BoundType::GeometryBVH: read_geometry(r, ptr, *b); break;
        case BoundType::Composite: read_composite(r, ptr, *b, seen); break;
        case BoundType::Capsule: case BoundType::Disc: case BoundType::Cylinder: b->unk70 = r.f32(ptr + 0x70); break;
        default: break; // sphere, box, cloth: the base struct holds everything
    }
    return b;
}

BoundPtr read_ybn(std::span<const uint8_t> file) {
    rsc::Resource res = rsc::Resource::load(file);
    if (res.version != rsc::kYbn) throw Error("ybn: resource version " + std::to_string(res.version) + ", expected 43 (an older layout is not read)");
    Reader r(res);
    return read_bound(r, rsc::kSystemBase);
}

// pgDictionary<phBound> (0x40): 0x20 hashes (ptr, count:u16, capacity:u16), 0x30 bounds (pointer list)
BoundDictionary read_ybd(std::span<const uint8_t> file) {
    rsc::Resource res = rsc::Resource::load(file);
    if (res.version != rsc::kYbn) throw Error("ybd: resource version " + std::to_string(res.version) + ", expected 43 (an older layout is not read)");
    Reader r(res);
    const uint64_t root = rsc::kSystemBase;
    BoundDictionary d;
    d.hashes = r.array<uint32_t>(r.u64(root + 0x20), r.u16(root + 0x28));
    std::map<uint64_t, std::shared_ptr<Bound>> seen;
    for (uint64_t p : r.array<uint64_t>(r.u64(root + 0x30), r.u16(root + 0x38))) d.bounds.push_back(read_bound(r, p, &seen));
    if (d.hashes.size() != d.bounds.size()) throw Error("ybd: " + std::to_string(d.hashes.size()) + " names for " + std::to_string(d.bounds.size()) + " bounds");
    return d;
}

Bytes write_ybd(const BoundDictionary& d) {
    if (d.hashes.size() != d.bounds.size()) throw Error("ybd: hashes and bounds differ in count");
    if (d.bounds.size() > 0xFFFF) throw Error("ybd: more than 65535 bounds");
    Builder b;
    BlockId root = b.alloc(Segment::System, 0x40);
    BlockId pages = b.alloc_pages_info();
    BlockId hashes = d.hashes.empty() ? BlockId{} : b.array(Segment::System, d.hashes);
    BlockId ptrs = d.bounds.empty() ? BlockId{} : b.alloc(Segment::System, d.bounds.size() * 8);
    std::map<const Bound*, BlockId> written;
    for (size_t i = 0; i < d.bounds.size(); i++)
        if (d.bounds[i]) b.pointer(ptrs, i * 8, write_bound(b, *d.bounds[i], &written));
    BlockWriter w(b, root);
    w.u32(0x405715A0);   // the dictionary's vtable in the game's file
    w.u32(1);
    w.ptr(pages);
    w.u64(0);
    w.u32(1);
    w.u32(0);
    const uint16_t n = (uint16_t)d.bounds.size();
    w.ptr(hashes); w.u16(n); w.u16(n); w.u32(0);
    w.ptr(ptrs); w.u16(n); w.u16(n); w.u32(0);
    return b.build(rsc::kYbn);
}

// ================================================================ writing

namespace {

constexpr uint32_t vft_of(BoundType t) {
    switch (t) {
        case BoundType::Sphere: return 1080221960u;
        case BoundType::Capsule: return 1080213112u;
        case BoundType::Box: return 1080221016u;
        case BoundType::Geometry: return 1080226408u;
        case BoundType::GeometryBVH: return 1080228536u;
        case BoundType::Composite: return 1080212136u;
        case BoundType::Disc: return 1080229960u;
        case BoundType::Cylinder: return 1080202872u;
        case BoundType::Cloth: return 1080229448u;
        default: return 1080226408u;
    }
}

size_t bound_size(BoundType t) {
    switch (t) {
        case BoundType::Capsule: case BoundType::Disc: case BoundType::Cylinder: return 128;
        case BoundType::Geometry: return 304;
        case BoundType::GeometryBVH: return 336;
        case BoundType::Composite: return 176;
        default: return 112;
    }
}

BlockId write_bvh(Builder& b, const BoundBVH& bvh) {
    BlockId blk = b.alloc(Segment::System, 128);
    BlockId nodes = b.array(Segment::System, bvh.nodes);
    BlockId trees = b.array(Segment::System, bvh.trees);
    BlockWriter w(b, blk);
    w.ptr(nodes);
    w.u32(bvh.node_count);
    w.u32((uint32_t)bvh.nodes.size());
    w.zero(16);
    w.vec4(bvh.bbox_min); w.vec4(bvh.bbox_max); w.vec4(bvh.bbox_center);
    w.vec4(bvh.quantum_inverse); w.vec4(bvh.quantum);
    w.ptr(trees);
    w.u16((uint16_t)bvh.trees.size());
    w.u16((uint16_t)bvh.trees.size());
    w.u32(0);
    return blk;
}

std::vector<int16_t> quantise(const std::vector<Vec3>& v, const Vec3& q) {
    std::vector<int16_t> out;
    out.reserve(v.size() * 3);
    auto qf = [](float x, float s) { return (int16_t)std::lround(std::clamp(s > 0 ? x / s : 0.f, -32767.f, 32767.f)); };
    for (auto& p : v) { out.push_back(qf(p.x, q.x)); out.push_back(qf(p.y, q.y)); out.push_back(qf(p.z, q.z)); }
    return out;
}

} // namespace

BlockId write_bound(Builder& b, const Bound& bound, std::map<const Bound*, BlockId>* written) {
    if (written) {
        auto it = written->find(&bound);
        if (it != written->end()) return it->second;
    }
    BlockId blk = b.alloc(Segment::System, bound_size(bound.type));
    if (written) (*written)[&bound] = blk;
    BlockWriter w(b, blk);
    w.u32(vft_of(bound.type));
    w.u32(1);
    w.u64(0);
    w.u8((uint8_t)bound.type);
    w.u8(0); w.u16(0);
    w.f32(bound.sphere_radius);
    w.u32(0); w.u32(0);
    w.vec3(bound.box_max);
    w.f32(bound.margin);
    w.vec3(bound.box_min);
    w.u32(bound.unk3C);
    w.vec3(bound.box_center);
    w.u8(bound.material_index); w.u8(bound.procedural_id); w.u8(bound.room_ped_density); w.u8(bound.unk_flags);
    w.vec3(bound.sphere_center);
    w.u8(bound.poly_flags); w.u8(bound.material_colour_index);
    w.u16(bound.unk5E);
    w.vec3(bound.inertia);
    w.f32(bound.volume);
    if (bound_size(bound.type) == 128) w.f32(bound.unk70);

    if ((bound.type == BoundType::Geometry || bound.type == BoundType::GeometryBVH) && bound.geometry) {
        auto& g = *bound.geometry;
        Vec3 q = g.quantum;
        if (q.x <= 0 || q.y <= 0 || q.z <= 0) q = (bound.box_max - bound.box_min) * (0.5f / 32767.f);
        BlockId shrunk = b.array(Segment::System, quantise(g.vertices_shrunk, q));
        BlockId verts = b.array(Segment::System, quantise(g.vertices, q));
        Bytes polys(g.polygons.size() * 16);
        for (size_t i = 0; i < g.polygons.size(); i++) {
            std::memcpy(polys.data() + i * 16, g.polygons[i].raw.data(), 16);
            polys[i * 16] = (uint8_t)((polys[i * 16] & 0xF8) | ((uint8_t)g.polygons[i].kind & 7));
        }
        BlockId polys_blk = b.array(Segment::System, polys);
        BlockId colours = b.array(Segment::System, g.vertex_colours);
        BlockId octants;
        if (g.has_octants) {
            size_t total = 0;
            for (auto& o : g.octants) total += o.size();
            octants = b.alloc(Segment::System, 128 + total * 4);
            BlockWriter ow(b, octants);
            for (auto& o : g.octants) ow.u32((uint32_t)o.size());
            size_t off = 96;
            for (auto& o : g.octants) { ow.ptr(octants, off); off += o.size() * 4; }
            for (auto& o : g.octants) for (auto v : o) ow.u32(v);
        }
        std::vector<BoundMaterial> mats = g.materials;
        if (!mats.empty() && mats.size() < 4) mats.resize(4);
        BlockId mats_blk = b.array(Segment::System, mats);
        BlockId matcol = b.array(Segment::System, g.material_colours);
        BlockId polymat = b.array(Segment::System, g.polygon_materials);
        BlockId bvh = (bound.type == BoundType::GeometryBVH && g.bvh) ? write_bvh(b, *g.bvh) : BlockId{};

        w.u64(0);                          // 0x70
        w.ptr(shrunk);
        w.u16(g.unk80); w.u16(g.unk82);
        w.u32((uint32_t)g.vertices_shrunk.size());
        w.ptr(polys_blk);
        w.vec3(q); w.f32(g.unk9C);
        w.vec3(g.center); w.f32(g.unkAC);
        w.ptr(verts);
        w.ptr(colours);
        w.ptr(octants);
        if (octants.valid()) w.ptr(octants, 32); else w.u64(0);
        w.u32((uint32_t)g.vertices.size());
        w.u32((uint32_t)g.polygons.size());
        w.zero(24);
        w.ptr(mats_blk);
        w.ptr(matcol);
        w.zero(24);
        w.ptr(polymat);
        w.u8((uint8_t)g.materials.size());
        w.u8((uint8_t)g.material_colours.size());
        w.u16(0);
        w.zero(12);                        // = 0x130
        if (bound.type == BoundType::GeometryBVH) {
            w.ptr(bvh);
            w.u64(0);
            w.u16(g.unk140);
            w.u16(0);
            w.zero(12);
        }
    } else if (bound.type == BoundType::Composite) {
        size_t n = bound.children.size();
        BlockId children, tr1, tr2, boxes, f1, f2;
        if (n) {
            children = b.alloc(Segment::System, n * 8);
            std::vector<BoundTransform> trs;
            std::vector<AABB> bbs;
            std::vector<uint32_t> fl1;
            for (size_t i = 0; i < n; i++) {
                auto& ch = bound.children[i];
                if (ch.bound) b.pointer(children, i * 8, write_bound(b, *ch.bound, written));
                trs.push_back(ch.transform);
                bbs.push_back(ch.bbox);
                fl1.push_back(ch.flags1); fl1.push_back(ch.flags2);
            }
            auto pack_transforms = [](const std::vector<BoundTransform>& v) { // xml: see BoundTransform::to_file
                std::vector<std::array<uint8_t, 64>> out(v.size());
                for (size_t i = 0; i < v.size(); i++) v[i].to_file(out[i].data());
                return out;
            };
            tr1 = b.array(Segment::System, pack_transforms(trs));
            tr2 = bound.has_child_transforms2 && bound.child_transforms2.size() == n ? b.array(Segment::System, pack_transforms(bound.child_transforms2)) : tr1;
            boxes = b.array(Segment::System, bbs);
            // the two flag arrays are one block in every composite of the game's .ybn files (1,514 of 1,514 looked at),
            // as the two transform arrays are when there is no second one
            if (bound.has_child_flags) f1 = f2 = b.array(Segment::System, fl1);
        }
        BlockId bvh = bound.composite_bvh ? write_bvh(b, *bound.composite_bvh) : BlockId{};
        w.ptr(children);
        w.ptr(tr1);
        w.ptr(tr2);
        w.ptr(boxes);
        w.ptr(f1);
        w.ptr(f2);
        w.u16((uint16_t)n);
        w.u16((uint16_t)n);
        w.u32(0);
        w.ptr(bvh);
    }
    return blk;
}

Bytes write_ybn(const Bound& bound) {
    Builder b;
    BlockId root = write_bound(b, bound);
    BlockId pages = b.alloc_pages_info();
    b.pointer(root, 8, pages);
    return b.build(rsc::kYbn);
}

// ================================================================ building new bounds

namespace {

struct BvhItem { Vec3 min, max; int index; };

struct BvhNode {
    std::vector<BvhNode> children;
    std::vector<BvhItem> items;
    bool leaf = true;
    Vec3 min, max;
    int index = 0;
    int total_nodes() const { int c = 1; for (auto& ch : children) c += ch.total_nodes(); return c; }
    int total_items() const { int c = (int)items.size(); for (auto& ch : children) c += ch.total_items(); return c; }
    void update_minmax() {
        min = {INFINITY, INFINITY, INFINITY}; max = {-INFINITY, -INFINITY, -INFINITY};
        for (auto& it : items) { min = vmin(min, it.min); max = vmax(max, it.max); }
        for (auto& ch : children) { ch.update_minmax(); min = vmin(min, ch.min); max = vmax(max, ch.max); }
    }
    void build(int threshold) {
        update_minmax();
        if ((int)items.size() <= threshold) return;
        Vec3 avg;
        for (auto& it : items) avg = avg + it.min + it.max;
        avg = avg * (0.5f / items.size());
        int cx = 0, cy = 0, cz = 0;
        for (auto& it : items) {
            Vec3 c = (it.min + it.max) * 0.5f;
            if (c.x < avg.x) cx++;
            if (c.y < avg.y) cy++;
            if (c.z < avg.z) cz++;
        }
        float target = items.size() / 2.f;
        float dx = std::fabs(target - cx), dy = std::fabs(target - cy), dz = std::fabs(target - cz);
        int axis = (dx <= dy && dx <= dz) ? 0 : (dy <= dz ? 1 : 2);
        std::vector<BvhItem> l1, l2;
        for (auto& it : items) {
            Vec3 c = (it.min + it.max) * 0.5f;
            bool s = axis == 0 ? c.x > avg.x : axis == 1 ? c.y > avg.y : c.z > avg.z;
            (s ? l1 : l2).push_back(it);
        }
        if (l1.empty() || l2.empty()) {
            std::vector<BvhItem> all = l1;
            all.insert(all.end(), l2.begin(), l2.end());
            if (all.empty()) return;
            std::stable_sort(all.begin(), all.end(), [](const BvhItem& a, const BvhItem& b) {
                if (a.min.x != b.min.x) return a.min.x < b.min.x;
                if (a.min.y != b.min.y) return a.min.y < b.min.y;
                return a.min.z < b.min.z;
            });
            size_t h = all.size() / 2;
            l1.assign(all.begin(), all.begin() + (ptrdiff_t)h);
            l2.assign(all.begin() + (ptrdiff_t)h, all.end());
            if (l1.empty() || l2.empty()) return;
        }
        items.clear();
        leaf = false;
        BvhNode n1, n2;
        n1.items = l1; n1.build(threshold);
        n2.items = l2; n2.build(threshold);
        children.push_back(std::move(n1));
        children.push_back(std::move(n2));
        std::stable_sort(children.begin(), children.end(), [](const BvhNode& a, const BvhNode& b) { return a.total_items() > b.total_items(); });
        update_minmax();
    }
    void gather_nodes(std::vector<BvhNode*>& nodes) {
        index = (int)nodes.size();
        nodes.push_back(this);
        for (auto& ch : children) ch.gather_nodes(nodes);
    }
    void gather_trees(std::vector<BvhNode*>& trees) {
        if (total_nodes() > 127 && !children.empty()) for (auto& ch : children) ch.gather_trees(trees);
        else trees.push_back(this);
    }
};

/// Port of CodeWalker's BVHBuilder: `items` are reordered (grouped by leaf) when threshold > 1,
/// and `order` receives the new index of each original item.
BoundBVH build_bvh(std::vector<BvhItem>& items, int threshold, std::vector<int>& order) {
    BoundBVH bvh;
    Vec3 mn{INFINITY, INFINITY, INFINITY}, mx{-INFINITY, -INFINITY, -INFINITY};
    for (auto& it : items) { mn = vmin(mn, it.min); mx = vmax(mx, it.max); }
    if (items.empty()) mn = mx = {};
    Vec3 cen = (mn + mx) * 0.5f;
    float nan = std::nanf("");
    bvh.bbox_min = Vec4(mn, nan);
    bvh.bbox_max = Vec4(mx, nan);
    bvh.bbox_center = Vec4(cen, nan);
    Vec3 ext = vmax({std::fabs(mn.x - cen.x), std::fabs(mn.y - cen.y), std::fabs(mn.z - cen.z)},
                    {std::fabs(mx.x - cen.x), std::fabs(mx.y - cen.y), std::fabs(mx.z - cen.z)});
    Vec3 q = ext * (1.f / 32767.f);
    bvh.quantum = Vec4(q, nan);
    bvh.quantum_inverse = Vec4({q.x ? 1 / q.x : 0, q.y ? 1 / q.y : 0, q.z ? 1 / q.z : 0}, nan);

    BvhNode root;
    root.items = items;
    root.build(threshold);
    std::vector<BvhNode*> nodes, trees;
    root.gather_nodes(nodes);
    root.gather_trees(trees);

    order.assign(items.size(), 0);
    if (threshold > 1) {
        std::vector<BvhItem> reordered;
        for (auto* n : nodes)
            for (auto& it : n->items) {
                order[it.index] = (int)reordered.size();
                it.index = (int)reordered.size();
                reordered.push_back(it);
            }
        items = reordered;
    } else {
        for (size_t i = 0; i < items.size(); i++) order[i] = (int)i;
    }
    Vec3 qi = bvh.quantum_inverse.xyz();
    auto qv = [&](const Vec3& v, int16_t out[3]) {
        Vec3 r = {(v.x - cen.x) * qi.x, (v.y - cen.y) * qi.y, (v.z - cen.z) * qi.z};
        out[0] = (int16_t)std::clamp(r.x, -32767.f, 32767.f);
        out[1] = (int16_t)std::clamp(r.y, -32767.f, 32767.f);
        out[2] = (int16_t)std::clamp(r.z, -32767.f, 32767.f);
    };
    for (auto* n : nodes) {
        BoundBVH::Node bn{};
        qv(n->min, bn.min);
        qv(n->max, bn.max);
        int tn = n->total_nodes();
        int id = n->items.empty() ? 0 : n->items[0].index;
        bn.item_count = (int16_t)(tn <= 1 ? n->total_items() : 0);
        bn.item_id = (int16_t)(tn <= 1 ? id : tn);
        bvh.nodes.push_back(bn);
    }
    for (auto* t : trees) {
        BoundBVH::Tree bt{};
        qv(t->min, bt.min);
        qv(t->max, bt.max);
        bt.node_first = (int16_t)t->index;
        bt.node_end = (int16_t)(t->index + t->total_nodes());
        bvh.trees.push_back(bt);
    }
    bvh.node_count = (uint32_t)bvh.nodes.size();
    if (threshold <= 1) {
        size_t cap = items.size() * 2 + 1;
        BoundBVH::Node empty{};
        empty.item_id = 1;
        while (bvh.nodes.size() < cap) bvh.nodes.push_back(empty);
    }
    return bvh;
}

} // namespace

void Bound::finalize() {
    if (type == BoundType::Composite) {
        Vec3 mn{INFINITY, INFINITY, INFINITY}, mx{-INFINITY, -INFINITY, -INFINITY};
        std::vector<BvhItem> items;
        for (size_t i = 0; i < children.size(); i++) {
            auto& ch = children[i];
            if (!ch.bound) continue;
            Mat4 m = ch.transform.matrix();
            Vec3 cmn{INFINITY, INFINITY, INFINITY}, cmx{-INFINITY, -INFINITY, -INFINITY};
            for (int k = 0; k < 8; k++) {
                Vec3 p{(k & 1) ? ch.bound->box_max.x : ch.bound->box_min.x, (k & 2) ? ch.bound->box_max.y : ch.bound->box_min.y, (k & 4) ? ch.bound->box_max.z : ch.bound->box_min.z};
                Vec3 t{p.x * m.at(0, 0) + p.y * m.at(1, 0) + p.z * m.at(2, 0) + m.at(3, 0),
                       p.x * m.at(0, 1) + p.y * m.at(1, 1) + p.z * m.at(2, 1) + m.at(3, 1),
                       p.x * m.at(0, 2) + p.y * m.at(1, 2) + p.z * m.at(2, 2) + m.at(3, 2)};
                cmn = vmin(cmn, t); cmx = vmax(cmx, t);
            }
            ch.bbox.min = Vec4(ch.bound->box_min, 1.401298E-45f);
            ch.bbox.max = Vec4(ch.bound->box_max, ch.bound->margin);
            mn = vmin(mn, cmn); mx = vmax(mx, cmx);
            items.push_back({cmn, cmx, (int)i});
        }
        if (items.empty()) mn = mx = {};
        box_min = mn; box_max = mx;
        box_center = (mn + mx) * 0.5f;
        sphere_center = box_center;
        sphere_radius = (mx - box_center).length();
        composite_bvh.reset();
        if (children.size() > 5) {
            std::vector<int> order;
            composite_bvh = build_bvh(items, 1, order);
        }
        return;
    }
    if (!geometry) return;
    auto& g = *geometry;
    for (size_t i = 0; i < g.polygons.size(); i++)
        for (int k = 0; k < g.polygons[i].vertex_count(); k++)
            if (g.polygons[i].vertex(k) >= g.vertices.size())
                throw Error("bound: polygon " + std::to_string(i) + " names vertex " + std::to_string(g.polygons[i].vertex(k)) + " of " + std::to_string(g.vertices.size()));
    // box and sphere around the vertices (relative to center)
    Vec3 mn{INFINITY, INFINITY, INFINITY}, mx{-INFINITY, -INFINITY, -INFINITY};
    for (auto& v : g.vertices) { mn = vmin(mn, v); mx = vmax(mx, v); }
    if (g.vertices.empty()) mn = mx = {};
    box_min = mn + g.center - Vec3{margin, margin, margin};
    box_max = mx + g.center + Vec3{margin, margin, margin};
    box_center = (box_min + box_max) * 0.5f;
    sphere_center = box_center;
    sphere_radius = (box_max - box_center).length();
    // quantum from the vertices' reach so nothing saturates; the one the geometry has (read from a file, or made earlier) stays while it still covers them,
    // so vertices nobody moved are written back as they were read
    Vec3 reach;
    for (auto& v : g.vertices) reach = vmax(reach, {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)});
    auto covers = [](float q, float r) { return q > 0 && r <= q * 32767.f; };
    if (!(covers(g.quantum.x, reach.x) && covers(g.quantum.y, reach.y) && covers(g.quantum.z, reach.z)))
        g.quantum = {std::max(reach.x, 1e-6f) * (1.0001f / 32767.f), std::max(reach.y, 1e-6f) * (1.0001f / 32767.f), std::max(reach.z, 1e-6f) * (1.0001f / 32767.f)};
    // materials table from the per-polygon indices (keep as is if consistent)
    if (g.polygon_materials.size() != g.polygons.size()) g.polygon_materials.assign(g.polygons.size(), 0);
    if (g.materials.empty()) g.materials.push_back(BoundMaterial{});
    // triangle areas and neighbour edges
    std::map<std::pair<uint16_t, uint16_t>, std::vector<std::pair<int, int>>> edges; // edge -> (poly, edge slot)
    for (size_t i = 0; i < g.polygons.size(); i++) {
        auto& p = g.polygons[i];
        if (p.kind != BoundPolygon::Triangle) continue;
        Vec3 a = g.vertices[p.vertex(0)], bb = g.vertices[p.vertex(1)], c = g.vertices[p.vertex(2)];
        Vec3 e1 = bb - a, e2 = c - a;
        Vec3 cr{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
        p.set_area(cr.length() * 0.5f);
        for (int k = 0; k < 3; k++) {
            uint16_t v1 = p.vertex(k), v2 = p.vertex((k + 1) % 3);
            edges[{std::min(v1, v2), std::max(v1, v2)}].push_back({(int)i, k});
            p.set_edge(k, -1);
        }
    }
    for (auto& [key, list] : edges) {
        if (list.size() < 2) continue;
        auto [p1, e1] = list[0];
        auto [p2, e2] = list[1];
        g.polygons[p1].set_edge(e1, p2);
        g.polygons[p2].set_edge(e2, p1);
    }
    // shrunk vertices (geometry type only): vertex moved inward along its averaged normal
    g.vertices_shrunk.clear();
    g.has_octants = false;
    if (type == BoundType::Geometry) {
        std::vector<Vec3> normals(g.vertices.size());
        for (auto& p : g.polygons) {
            if (p.kind != BoundPolygon::Triangle) continue;
            Vec3 a = g.vertices[p.vertex(0)], bb = g.vertices[p.vertex(1)], c = g.vertices[p.vertex(2)];
            Vec3 e1 = a - bb, e2 = c - bb;
            Vec3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
            float l = n.length();
            if (l > 0) n = n * (1 / l);
            for (int k = 0; k < 3; k++) normals[p.vertex(k)] = normals[p.vertex(k)] + n;
        }
        for (size_t i = 0; i < g.vertices.size(); i++) {
            float l = normals[i].length();
            Vec3 n = l > 0 ? normals[i] * (1 / l) : Vec3{};
            g.vertices_shrunk.push_back(g.vertices[i] - n * margin);
        }
        // octants: for each of the 8 directions, the vertices not shadowed by another
        g.has_octants = true;
        static const Vec3 flip[8] = {{1, 1, 1}, {-1, 1, 1}, {1, -1, 1}, {-1, -1, 1}, {1, 1, -1}, {-1, 1, -1}, {1, -1, -1}, {-1, -1, -1}};
        for (int o = 0; o < 8; o++) {
            auto shadowed = [&](const Vec3& v1, const Vec3& v2) {
                Vec3 d = v2 - v1;
                d = {d.x * flip[o].x, d.y * flip[o].y, d.z * flip[o].z};
                return d.x >= 0 && d.y >= 0 && d.z >= 0;
            };
            std::vector<uint32_t> kept;
            for (uint32_t i = 0; i < g.vertices_shrunk.size(); i++) {
                bool add = true;
                std::vector<uint32_t> next;
                for (auto j : kept) {
                    if (shadowed(g.vertices_shrunk[i], g.vertices_shrunk[j])) { add = false; next = kept; break; }
                    if (!shadowed(g.vertices_shrunk[j], g.vertices_shrunk[i])) next.push_back(j);
                }
                if (add) next.push_back(i);
                kept = next;
            }
            g.octants[o] = kept;
        }
    }
    g.bvh.reset();
    if (type == BoundType::GeometryBVH && !g.polygons.empty()) {
        std::vector<BvhItem> items;
        for (size_t i = 0; i < g.polygons.size(); i++) {
            auto& p = g.polygons[i];
            Vec3 pmn{INFINITY, INFINITY, INFINITY}, pmx{-INFINITY, -INFINITY, -INFINITY};
            for (int k = 0; k < p.vertex_count(); k++) {
                Vec3 v = g.vertices[p.vertex(k)] + g.center;
                pmn = vmin(pmn, v); pmx = vmax(pmx, v);
            }
            float r = p.radius();
            pmn = pmn - Vec3{r, r, r}; pmx = pmx + Vec3{r, r, r};
            items.push_back({pmn, pmx, (int)i});
        }
        std::vector<int> order;
        BoundBVH bvh = build_bvh(items, 4, order);
        // polygons are grouped by leaf: reorder them and fix neighbour indices
        std::vector<BoundPolygon> polys(g.polygons.size());
        std::vector<uint8_t> mats(g.polygons.size());
        for (size_t i = 0; i < g.polygons.size(); i++) {
            polys[order[i]] = g.polygons[i];
            mats[order[i]] = g.polygon_materials[i];
        }
        for (auto& p : polys)
            if (p.kind == BoundPolygon::Triangle)
                for (int k = 0; k < 3; k++) { uint16_t e = p.edge(k); p.set_edge(k, e == 0xFFFF ? -1 : order[e]); }
        g.polygons = polys;
        g.polygon_materials = mats;
        box_min = bvh.bbox_min.xyz(); box_max = bvh.bbox_max.xyz(); box_center = bvh.bbox_center.xyz();
        sphere_center = box_center;
        sphere_radius = (box_max - box_center).length();
        g.bvh = std::move(bvh);
    }
}

BoundPtr Bound::make_mesh(const std::vector<Vec3>& vertices, const std::vector<uint32_t>& triangles, const BoundMaterial& material, bool bvh) {
    auto b = std::make_shared<Bound>();
    b->type = bvh ? BoundType::GeometryBVH : BoundType::Geometry;
    Bound::Geometry g;
    Vec3 mn{INFINITY, INFINITY, INFINITY}, mx{-INFINITY, -INFINITY, -INFINITY};
    for (auto& v : vertices) { mn = vmin(mn, v); mx = vmax(mx, v); }
    if (vertices.empty()) mn = mx = {};
    g.center = (mn + mx) * 0.5f;
    for (auto& v : vertices) g.vertices.push_back(v - g.center);
    for (size_t i = 0; i + 2 < triangles.size(); i += 3)
        g.polygons.push_back(BoundPolygon::triangle((uint16_t)triangles[i], (uint16_t)triangles[i + 1], (uint16_t)triangles[i + 2]));
    g.materials = {material};
    g.polygon_materials.assign(g.polygons.size(), 0);
    b->geometry = std::move(g);
    b->material_index = material.type();
    b->margin = 0.005f;
    b->finalize();
    return b;
}

BoundPtr Bound::make_composite(std::vector<BoundChild> children) {
    auto b = std::make_shared<Bound>();
    b->type = BoundType::Composite;
    b->children = std::move(children);
    b->finalize();
    return b;
}

// ---------------------------------------------------------------------------------------- triangles

namespace {

struct TriSink {
    std::vector<BoundTriangle>& out;
    const AABB* clip;
    int seg;
    Vec3 col[4]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}};   // the current transform: p' = col0 x + col1 y + col2 z + col3

    Vec3 xf(const Vec3& p) const { return col[0] * p.x + col[1] * p.y + col[2] * p.z + col[3]; }
    void tri(const Vec3& a, const Vec3& b, const Vec3& c, const BoundMaterial& m) {
        BoundTriangle t{{xf(a), xf(b), xf(c)}, m};
        if (clip) {
            const Vec3 mn = vmin(vmin(t.v[0], t.v[1]), t.v[2]), mx = vmax(vmax(t.v[0], t.v[1]), t.v[2]);
            if (mx.x < clip->min.x || mn.x > clip->max.x || mx.y < clip->min.y || mn.y > clip->max.y || mx.z < clip->min.z || mn.z > clip->max.z) return;
        }
        out.push_back(t);
    }
    void quad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const BoundMaterial& m) { tri(a, b, c, m); tri(a, c, d, m); }
    /// A box from a corner and three edges.
    void box(const Vec3& o, const Vec3& e1, const Vec3& e2, const Vec3& e3, const BoundMaterial& m) {
        const Vec3 p[8] = {o, o + e1, o + e2, o + e1 + e2, o + e3, o + e1 + e3, o + e2 + e3, o + e1 + e2 + e3};
        quad(p[0], p[2], p[3], p[1], m); quad(p[4], p[5], p[7], p[6], m);
        quad(p[0], p[1], p[5], p[4], m); quad(p[2], p[6], p[7], p[3], m);
        quad(p[0], p[4], p[6], p[2], m); quad(p[1], p[3], p[7], p[5], m);
    }
    static Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    /// Two unit vectors perpendicular to the unit vector n.
    static void basis(const Vec3& n, Vec3& u, Vec3& v) {
        const Vec3 h = std::fabs(n.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        u = cross(h, n);
        u = u * (1.f / std::max(u.length(), 1e-12f));
        v = cross(n, u);
    }
    void sphere(const Vec3& c, float r, const BoundMaterial& m) {
        const int rings = std::max(2, seg / 2);
        auto at = [&](int i, int j) {
            const float th = 3.14159265f * i / rings, ph = 6.2831853f * j / seg;
            return c + Vec3{std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)} * r;
        };
        for (int i = 0; i < rings; i++)
            for (int j = 0; j < seg; j++) quad(at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1), m);
    }
    void cylinder(const Vec3& a, const Vec3& b, float r, bool caps, const BoundMaterial& m) {
        Vec3 n = b - a;
        const float len = n.length();
        if (len < 1e-6f) return;
        n = n * (1.f / len);
        Vec3 u, v;
        basis(n, u, v);
        for (int j = 0; j < seg; j++) {
            const float p0 = 6.2831853f * j / seg, p1 = 6.2831853f * (j + 1) / seg;
            const Vec3 d0 = (u * std::cos(p0) + v * std::sin(p0)) * r, d1 = (u * std::cos(p1) + v * std::sin(p1)) * r;
            quad(a + d0, a + d1, b + d1, b + d0, m);
            if (caps) { tri(a, a + d1, a + d0, m); tri(b, b + d0, b + d1, m); }
        }
    }

    /// One polygon of a geometry bound as triangles (a primitive as a coarse mesh).
    void polygon(const Bound::Geometry& g, size_t i) {
        auto V = [&](uint16_t k) { return k < g.vertices.size() ? g.vertices[k] + g.center : g.center; };
        const BoundPolygon& p = g.polygons[i];
        BoundMaterial m;
        if (i < g.polygon_materials.size() && g.polygon_materials[i] < g.materials.size()) m = g.materials[g.polygon_materials[i]];
        switch (p.kind) {
            case BoundPolygon::Triangle: tri(V(p.vertex(0)), V(p.vertex(1)), V(p.vertex(2)), m); break;
            case BoundPolygon::Box: {   // four corners on the box's diagonals (CodeWalker's reading)
                const Vec3 p1 = V(p.vertex(0)), p2 = V(p.vertex(1)), p3 = V(p.vertex(2)), p4 = V(p.vertex(3));
                const Vec3 a1 = ((p3 + p4) - (p1 + p2)) * 0.5f;
                box(p1, a1, (p3 - a1) - p1, (p4 - a1) - p1, m);
                break;
            }
            case BoundPolygon::Sphere: sphere(V(p.vertex(0)), p.radius(), m); break;
            case BoundPolygon::Capsule:
                cylinder(V(p.vertex(0)), V(p.vertex(1)), p.radius(), false, m);
                sphere(V(p.vertex(0)), p.radius(), m);
                sphere(V(p.vertex(1)), p.radius(), m);
                break;
            case BoundPolygon::Cylinder: cylinder(V(p.vertex(0)), V(p.vertex(1)), p.radius(), true, m); break;
        }
    }

    void bound(const Bound& b) {
        const BoundMaterial whole{b.material_index, 0};
        switch (b.type) {
            case BoundType::Composite: {
                const Vec3 saved[4] = {col[0], col[1], col[2], col[3]};
                for (const BoundChild& ch : b.children) {
                    if (!ch.bound) continue;
                    const BoundTransform& t = ch.transform;
                    for (int k = 0; k < 3; k++) col[k] = saved[0] * t.col[k].x + saved[1] * t.col[k].y + saved[2] * t.col[k].z;
                    col[3] = saved[0] * t.col[3].x + saved[1] * t.col[3].y + saved[2] * t.col[3].z + saved[3];
                    bound(*ch.bound);
                }
                for (int k = 0; k < 4; k++) col[k] = saved[k];
                return;
            }
            case BoundType::Geometry:
            case BoundType::GeometryBVH: {
                if (!b.geometry) return;
                const Bound::Geometry& g = *b.geometry;
                for (size_t i = 0; i < g.polygons.size(); i++) polygon(g, i);
                return;
            }
            case BoundType::Sphere: sphere(b.sphere_center, b.sphere_radius, whole); return;
            default:   // box, capsule, cylinder, disc as whole bounds: their box
                box(b.box_min, {b.box_max.x - b.box_min.x, 0, 0}, {0, b.box_max.y - b.box_min.y, 0}, {0, 0, b.box_max.z - b.box_min.z}, whole);
                return;
        }
    }
};

}   // namespace

std::vector<BoundTriangle> bound_triangles(const Bound& b, int segments, const AABB* clip) {
    std::vector<BoundTriangle> out;
    TriSink sink{out, clip, std::max(3, segments)};
    sink.bound(b);
    return out;
}

std::vector<BoundTriangle> polygon_triangles(const Bound::Geometry& g, size_t polygon, int segments) {
    std::vector<BoundTriangle> out;
    if (polygon >= g.polygons.size()) return out;
    TriSink sink{out, nullptr, std::max(3, segments)};
    sink.polygon(g, polygon);
    return out;
}

} // namespace rk
