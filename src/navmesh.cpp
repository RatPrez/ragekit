#include "ragekit/navmesh.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "ragekit/error.h"
#include "ragekit/rsc.h"

namespace rk {

NavSector::NavSector(const NavSector& o)
    : min(o.min), max(o.max), box(o.box), has_data(o.has_data), points_start(o.points_start), polys(o.polys), points(o.points) {
    for (size_t i = 0; i < 4; i++) if (o.children[i]) children[i] = std::make_unique<NavSector>(*o.children[i]);
}
NavSector& NavSector::operator=(const NavSector& o) {
    if (this != &o) { NavSector c(o); *this = std::move(c); }
    return *this;
}

Vec3 NavMesh::box_min() const { return sectors ? sectors->min.xyz() : size * -0.5f; }
Vec3 NavMesh::box_max() const { return sectors ? sectors->max.xyz() : size * 0.5f; }

Vec3 NavMesh::position(const NavVertex& v) const {
    const Vec3 lo = box_min(), hi = box_max();
    const float k = 1.0f / 65535.0f;
    return {lo.x + (hi.x - lo.x) * (v.x * k), lo.y + (hi.y - lo.y) * (v.y * k), lo.z + (hi.z - lo.z) * (v.z * k)};
}

NavVertex NavMesh::quantize(const Vec3& p) const {
    const Vec3 lo = box_min(), hi = box_max();
    auto q = [](float v, float a, float b) {
        const float t = b > a ? (v - a) / (b - a) : 0.0f;
        return (uint16_t)std::lround(std::clamp(t, 0.0f, 1.0f) * 65535.0f);
    };
    return {q(p.x, lo.x, hi.x), q(p.y, lo.y, hi.y), q(p.z, lo.z, hi.z)};
}

std::vector<Vec3> NavMesh::poly_vertices(size_t poly) const {
    std::vector<Vec3> out;
    if (poly >= polys.size()) return out;
    const NavPoly& p = polys[poly];
    for (uint32_t i = 0; i < p.vertex_count(); i++) {
        const size_t k = (size_t)p.first_index + i;
        if (k < indices.size() && indices[k] < vertices.size()) out.push_back(position(vertices[indices[k]]));
    }
    return out;
}

namespace {
template <class F> void each_sector(const NavSector& s, F&& f) {
    f(s);
    for (auto& c : s.children) if (c) each_sector(*c, f);
}
}   // namespace

std::vector<NavPoint> NavMesh::points() const {
    std::vector<NavPoint> out;
    if (sectors) each_sector(*sectors, [&](const NavSector& s) { out.insert(out.end(), s.points.begin(), s.points.end()); });
    return out;
}

uint32_t NavMesh::total_bytes() const {
    uint32_t n = (uint32_t)(vertices.size() * 6 + indices.size() * 2 + edges.size() * 8 + polys.size() * 48 + portals.size() * 28);
    if (sectors)
        each_sector(*sectors, [&](const NavSector& s) {
            n += 96;
            if (s.has_data) n += 32 + (uint32_t)s.polys.size() * 2 + (uint32_t)s.points.size() * 8;
        });
    return n;
}

// ---------------------------------------------------------------- reading

namespace {

// A list of items split into parts (rage's navmesh list): 48 bytes; the parts are 16 bytes each (pointer, count).
template <class T> std::vector<T> read_list(const Reader& r, uint64_t ptr) {
    std::vector<T> out;
    if (!ptr) return out;
    const uint32_t count = r.u32(ptr + 8);
    const uint64_t parts = r.u64(ptr + 0x10);
    const uint32_t nparts = r.u32(ptr + 0x20);
    for (uint32_t p = 0; p < nparts; p++) {
        const uint64_t items = r.u64(parts + 16ull * p);
        const uint32_t n = r.u32(parts + 16ull * p + 8);
        auto v = r.array<T>(items, n);
        out.insert(out.end(), v.begin(), v.end());
    }
    if (out.size() != count) throw Error("ynv: a list's parts hold " + std::to_string(out.size()) + " items, its header " + std::to_string(count));
    return out;
}

NavSector read_sector(const Reader& r, uint64_t ptr, int depth) {
    if (depth > 16) throw Error("ynv: sector tree too deep");
    NavSector s;
    s.min = r.at<Vec4>(ptr);
    s.max = r.at<Vec4>(ptr + 16);
    s.box = r.at<NavBox>(ptr + 32);
    const uint64_t data = r.u64(ptr + 0x2C);   // unaligned: right after the 12 byte box
    for (size_t i = 0; i < 4; i++) {
        const uint64_t c = r.u64(ptr + 0x34 + 8 * i);
        if (c) s.children[i] = std::make_unique<NavSector>(read_sector(r, c, depth + 1));
    }
    if (data) {
        s.has_data = true;
        s.points_start = r.u32(data);
        const uint64_t ids = r.u64(data + 8), pts = r.u64(data + 0x10);
        s.polys = r.array<uint16_t>(ids, r.u16(data + 0x18));
        s.points = r.array<NavPoint>(pts, r.u16(data + 0x1A));
    }
    return s;
}

static_assert(sizeof(NavVertex) == 6 && sizeof(NavEdge) == 8 && sizeof(NavPoly) == 48 && sizeof(NavPoint) == 8 && sizeof(NavPortal) == 28 &&
              sizeof(NavBox) == 12, "navmesh structures must match the file");

}   // namespace

NavMesh read_ynv(std::span<const uint8_t> file) {
    rsc::Resource res = rsc::Resource::load(file);
    Reader r(res);
    const uint64_t root = rsc::kSystemBase;
    NavMesh n;
    n.content_flags = r.u32(root + 0x10);
    n.version = r.u32(root + 0x14);
    std::memcpy(n.transform.data(), res.at(root + 0x20, 64), 64);
    n.size = r.at<Vec3>(root + 0x60);
    n.size_w = r.u32(root + 0x6C);
    n.vertices = read_list<NavVertex>(r, r.u64(root + 0x70));
    n.indices = read_list<uint16_t>(r, r.u64(root + 0x80));
    n.edges = read_list<NavEdge>(r, r.u64(root + 0x88));
    const uint32_t adj = r.u32(root + 0x94);
    if (adj > 32) throw Error("ynv: more than 32 adjacent areas");
    n.adjacent_areas = r.array<uint32_t>(root + 0x98, adj);
    n.polys = read_list<NavPoly>(r, r.u64(root + 0x118));
    if (const uint64_t s = r.u64(root + 0x120)) n.sectors = read_sector(r, s, 0);
    n.portals = r.array<NavPortal>(r.u64(root + 0x128), r.u32(root + 0x14C));
    n.portal_links = r.array<uint16_t>(r.u64(root + 0x130), r.u32(root + 0x150));
    n.area_id = r.u32(root + 0x140);
    n.build = r.u32(root + 0x160);
    if (n.indices.size() != n.edges.size()) throw Error("ynv: indices and edges differ in count");
    return n;
}

// ---------------------------------------------------------------- writing

namespace {

// the vtables the game's files carry (any value works: the game sets its own)
constexpr uint32_t kNavMeshVft = 0x4061E7E8, kVertexListVft = 0x4061E8F8, kIndexListVft = 0x4061E8D8, kEdgeListVft = 0x4061E8E8,
                   kPolyListVft = 0x4061E8C8;

template <class T> BlockId write_list(Builder& b, const std::vector<T>& items, uint32_t vft) {
    // parts of 16 KB, as every list of the game's files
    const size_t per = 16384 / sizeof(T);
    const size_t nparts = (items.size() + per - 1) / per;
    BlockId list = b.alloc(Segment::System, 48);
    BlockId parts = nparts ? b.alloc(Segment::System, nparts * 16) : BlockId{};
    std::vector<uint32_t> offsets;
    for (size_t p = 0; p < nparts; p++) {
        const size_t first = p * per, n = std::min(per, items.size() - first);
        BlockId data = b.array(Segment::System, items.data() + first, n * sizeof(T));
        b.pointer(parts, p * 16, data);
        const uint32_t count = (uint32_t)n;
        b.put(parts, p * 16 + 8, count);
        offsets.push_back((uint32_t)first);
    }
    BlockId offs = offsets.empty() ? BlockId{} : b.array(Segment::System, offsets);
    BlockWriter w(b, list);
    w.u32(vft);
    w.u32(1);
    w.u32((uint32_t)items.size());
    w.u32(0);
    w.ptr(parts);
    w.ptr(offs);
    w.u32((uint32_t)nparts);
    return list;
}

BlockId write_sector(Builder& b, const NavSector& s) {
    BlockId blk = b.alloc(Segment::System, 96);
    BlockId data;
    if (s.has_data) {
        if (s.polys.size() > 0xFFFF || s.points.size() > 0xFFFF) throw Error("ynv: a sector holds at most 65535 polygons and points");
        data = b.alloc(Segment::System, 32);
        BlockId ids = s.polys.empty() ? BlockId{} : b.array(Segment::System, s.polys);
        BlockId pts = s.points.empty() ? BlockId{} : b.array(Segment::System, s.points);
        BlockWriter w(b, data);
        w.u32(s.points_start);
        w.u32(0);
        w.ptr(ids);
        w.ptr(pts);
        w.u16((uint16_t)s.polys.size());
        w.u16((uint16_t)s.points.size());
    }
    BlockWriter w(b, blk);
    w.vec4(s.min);
    w.vec4(s.max);
    w.bytes(&s.box, sizeof s.box);
    w.ptr(data);
    for (auto& c : s.children) w.ptr(c ? write_sector(b, *c) : BlockId{});
    return blk;
}

}   // namespace

Bytes write_ynv(const NavMesh& n) {
    if (n.indices.size() != n.edges.size()) throw Error("ynv: indices and edges differ in count");
    if (n.adjacent_areas.size() > 32) throw Error("ynv: at most 32 adjacent areas");
    Builder b;
    BlockId root = b.alloc(Segment::System, 368);
    b.pointer(root, 0x08, b.alloc_pages_info());
    const BlockId verts = n.vertices.empty() ? BlockId{} : write_list(b, n.vertices, kVertexListVft);
    const BlockId inds = n.indices.empty() ? BlockId{} : write_list(b, n.indices, kIndexListVft);
    const BlockId edges = n.edges.empty() ? BlockId{} : write_list(b, n.edges, kEdgeListVft);
    const BlockId polys = n.polys.empty() ? BlockId{} : write_list(b, n.polys, kPolyListVft);
    const BlockId sectors = n.sectors ? write_sector(b, *n.sectors) : BlockId{};
    const BlockId portals = n.portals.empty() ? BlockId{} : b.array(Segment::System, n.portals);
    const BlockId links = n.portal_links.empty() ? BlockId{} : b.array(Segment::System, n.portal_links);
    BlockWriter w(b, root);
    w.u32(kNavMeshVft);
    w.u32(1);
    w.seek(0x10);
    w.u32(n.content_flags);
    w.u32(n.version);
    w.u64(0);
    w.bytes(n.transform.data(), 64);
    w.vec3(n.size);
    w.u32(n.size_w);
    w.ptr(verts);
    w.u64(0);
    w.ptr(inds);
    w.ptr(edges);
    w.u32((uint32_t)n.indices.size());
    w.u32((uint32_t)n.adjacent_areas.size());
    for (size_t i = 0; i < 32; i++) w.u32(i < n.adjacent_areas.size() ? n.adjacent_areas[i] : 0);
    w.ptr(polys);
    w.ptr(sectors);
    w.ptr(portals);
    w.ptr(links);
    w.u32((uint32_t)n.vertices.size());
    w.u32((uint32_t)n.polys.size());
    w.u32(n.area_id);
    w.u32(n.total_bytes());
    w.u32((uint32_t)n.points().size());
    w.u32((uint32_t)n.portals.size());
    w.u32((uint32_t)n.portal_links.size());
    w.zero(12);
    w.u32(n.build);
    return b.build(rsc::kYnv);
}

} // namespace rk
