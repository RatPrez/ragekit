#include "ragekit/navmesh.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
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

// ---------------------------------------------------------------- editing

NavMesh::PolyShape NavMesh::shape(size_t poly) const {
    PolyShape s;
    if (poly >= polys.size()) return s;
    s.poly = polys[poly];
    s.corners = poly_vertices(poly);
    const size_t first = s.poly.first_index, n = s.poly.vertex_count();
    if (first + n <= edges.size()) s.edges.assign(edges.begin() + first, edges.begin() + first + n);
    s.source = (int32_t)poly;
    return s;
}

std::vector<NavMesh::PolyShape> NavMesh::shapes() const {
    std::vector<PolyShape> out;
    out.reserve(polys.size());
    for (size_t i = 0; i < polys.size(); i++) out.push_back(shape(i));
    return out;
}

namespace {
void leaves_of(NavSector& s, std::vector<NavSector*>& out) {
    if (s.has_data) out.push_back(&s);
    for (auto& c : s.children) if (c) leaves_of(*c, out);
}
int16_t quarter(float v) { return (int16_t)std::clamp(std::trunc(v * 4.0f), -32768.0f, 32767.0f); }
}   // namespace

void NavMesh::set_polys(const std::vector<PolyShape>& in) {
    if (in.size() > 0x3FFF) throw Error("ynv: at most 16383 polygons (a link names one in 14 bits)");
    const std::vector<NavVertex> old_vertices = vertices;
    const std::vector<uint16_t> old_indices = indices;
    const std::vector<NavPoly> old_polys = polys;
    std::vector<NavSector*> leaves;
    if (sectors) leaves_of(*sectors, leaves);
    std::vector<std::vector<uint32_t>> was_in(old_polys.size());   // original polygon -> its leaves
    for (uint32_t l = 0; l < leaves.size(); l++)
        for (uint16_t p : leaves[l]->polys) if (p < was_in.size()) was_in[p].push_back(l);

    // the stored vertices stay where they are (a few cells hold some no polygon uses, or not in first-use order);
    // new corners join them, and vertices an edit leaves unused go
    std::vector<NavVertex> nv = old_vertices;
    std::vector<uint16_t> ni;
    std::vector<NavEdge> ne;
    std::vector<NavPoly> np;
    std::unordered_map<uint64_t, uint16_t> at;
    for (size_t i = nv.size(); i-- > 0;) at[nv[i].x | (uint64_t)nv[i].y << 16 | (uint64_t)nv[i].z << 32] = (uint16_t)i;
    auto add = [&](const NavVertex& v) {
        const uint64_t k = v.x | (uint64_t)v.y << 16 | (uint64_t)v.z << 32;
        auto [it, fresh] = at.try_emplace(k, (uint16_t)nv.size());
        if (fresh) {
            if (nv.size() >= 0xFFFF) throw Error("ynv: more than 65535 vertices");
            nv.push_back(v);
        }
        ni.push_back(it->second);
    };
    const Vec3 lo = box_min(), hi = box_max();
    uint32_t links_so_far = 0;
    std::vector<std::vector<uint32_t>> leaf_polys(leaves.size());
    for (size_t k = 0; k < in.size(); k++) {
        const PolyShape& s = in[k];
        NavPoly p = s.poly;
        const bool kept = s.source >= 0 && (size_t)s.source < old_polys.size();
        const size_t n = kept ? old_polys[s.source].vertex_count() : s.corners.size();
        if (n == 0 || n > 0x7FF) throw Error("ynv: polygon " + std::to_string(k) + " has " + std::to_string(n) + " corners");
        if (s.edges.size() != n) throw Error("ynv: polygon " + std::to_string(k) + " has " + std::to_string(s.edges.size()) + " edges for " + std::to_string(n) + " corners");
        if (ni.size() + n > 0xFFFF) throw Error("ynv: more than 65535 polygon corners");
        p.first_index = (uint16_t)ni.size();
        p.set_vertex_count((uint32_t)n);
        if (kept) {
            const NavPoly& o = old_polys[s.source];
            for (size_t i = 0; i < n; i++) ni.push_back(old_indices[o.first_index + i]);
            for (uint32_t l : was_in[s.source]) leaf_polys[l].push_back((uint32_t)k);
        } else {
            Vec3 mn = s.corners[0], mx = s.corners[0], sum{};
            for (const Vec3& c : s.corners) {
                if (c.x < lo.x - 0.01f || c.y < lo.y - 0.01f || c.z < lo.z - 0.01f || c.x > hi.x + 0.01f || c.y > hi.y + 0.01f || c.z > hi.z + 0.01f)
                    throw Error("ynv: polygon " + std::to_string(k) + " reaches outside the cell's box");
                add(quantize(c));
                mn = vmin(mn, c); mx = vmax(mx, c); sum = sum + c;
            }
            p.box = {quarter(mn.x), quarter(mx.x), quarter(mn.y), quarter(mx.y), quarter(mn.z), quarter(mx.z)};
            const Vec3 mean = sum * (1.0f / (float)n);
            auto frac = [](float v, float a, float b) { return (uint32_t)std::clamp((int)std::lround((v - a) / std::max(b - a, 1e-3f) * 255.0f), 0, 255); };
            p.flags2 = (p.flags2 & 0xFFFF0000u) | frac(mean.x, mn.x, mx.x) | frac(mean.y, mn.y, mx.y) << 8;
            if (p.portal_link_count() == 0) p.part_flags = (p.part_flags & 0x7FFF) | links_so_far << 15;
            for (uint32_t l = 0; l < leaves.size(); l++) {
                const NavBox& b = leaves[l]->box;
                if (p.box.min_x <= b.max_x && p.box.max_x >= b.min_x && p.box.min_y <= b.max_y && p.box.max_y >= b.min_y) leaf_polys[l].push_back((uint32_t)k);
            }
        }
        links_so_far += p.portal_link_count();
        ne.insert(ne.end(), s.edges.begin(), s.edges.end());
        np.push_back(p);
    }
    std::vector<uint8_t> used(nv.size()), was_used(nv.size());
    for (uint16_t i : ni) used[i] = 1;
    for (uint16_t i : old_indices) if (i < was_used.size()) was_used[i] = 1;
    std::vector<uint16_t> remap(nv.size());
    size_t kept_vertices = 0;
    for (size_t i = 0; i < nv.size(); i++) {
        remap[i] = (uint16_t)kept_vertices;
        if (used[i] || !was_used[i]) nv[kept_vertices++] = nv[i];
    }
    if (kept_vertices != nv.size()) {
        nv.resize(kept_vertices);
        for (uint16_t& i : ni) i = remap[i];
    }
    vertices = std::move(nv);
    indices = std::move(ni);
    edges = std::move(ne);
    polys = std::move(np);
    for (size_t l = 0; l < leaves.size(); l++) {
        if (leaf_polys[l].size() > 0xFFFF) throw Error("ynv: a sector holds at most 65535 polygons");
        leaves[l]->polys.assign(leaf_polys[l].begin(), leaf_polys[l].end());
    }
}

void NavMesh::renumber_points() {
    if (!sectors) return;
    std::vector<NavSector*> leaves;
    leaves_of(*sectors, leaves);
    uint32_t n = 0;
    for (NavSector* l : leaves) { l->points_start = n; n += (uint32_t)l->points.size(); }
}

namespace {
bool close3(const Vec3& a, const Vec3& b) { return std::fabs(a.x - b.x) < 0.05f && std::fabs(a.y - b.y) < 0.05f && std::fabs(a.z - b.z) < 0.1f; }
// a-b and c-d lie on one line and overlap by more than 5 cm
bool overlap(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {
    const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return false;
    const float ux = dx / len, uy = dy / len;
    auto off = [&](const Vec3& p) { return std::fabs((p.x - a.x) * uy - (p.y - a.y) * ux); };
    if (off(c) > 0.05f || off(d) > 0.05f) return false;
    const float tc = (c.x - a.x) * ux + (c.y - a.y) * uy, td = (d.x - a.x) * ux + (d.y - a.y) * uy;
    return std::min(len, std::max(tc, td)) - std::max(0.0f, std::min(tc, td)) > 0.05f;
}
}   // namespace

std::vector<std::string> NavMesh::check_links(std::span<const uint32_t> only) const {
    std::vector<std::string> bad;
    std::vector<uint8_t> in(polys.size(), only.empty() ? 1 : 0);
    for (uint32_t p : only) if (p < in.size()) in[p] = 1;
    auto area_of = [&](const NavEdgePart& e) { return e.area_index() < adjacent_areas.size() ? adjacent_areas[e.area_index()] : 0xFFFFFFFFu; };
    std::vector<std::vector<Vec3>> corners(polys.size());
    for (size_t i = 0; i < polys.size(); i++) corners[i] = poly_vertices(i);
    for (size_t p = 0; p < polys.size(); p++) {
        const auto& vs = corners[p];
        for (size_t k = 0; k < vs.size(); k++) {
            const NavEdgePart& e = edges[polys[p].first_index + k].a;
            if (e.open() || area_of(e) != area_id || e.link_kind() == 2 || (e.space() & 1)) continue;
            const uint32_t q = e.poly();
            if (!in[p] && !(q < in.size() && in[q])) continue;
            const std::string side = "polygon " + std::to_string(p) + " side " + std::to_string(k);
            if (q >= polys.size()) { bad.push_back(side + " names polygon " + std::to_string(q) + ", past the end"); continue; }
            const Vec3 a = vs[k], b = vs[(k + 1) % vs.size()];
            const auto& ws = corners[q];
            // every side of q along this one (a zero-width stitch polygon has two): one must link back
            bool shared = false, back = false;
            std::string other;
            for (size_t j = 0; j < ws.size() && !back; j++) {
                const Vec3 c = ws[j], d = ws[(j + 1) % ws.size()];
                if (!((close3(c, b) && close3(d, a)) || (close3(c, a) && close3(d, b)) || overlap(a, b, c, d))) continue;
                shared = true;
                const NavEdgePart& be = edges[polys[q].first_index + j].a;
                if (!be.open() && area_of(be) == area_id && be.poly() == p) back = true;
                else if (other.empty()) other = be.open() ? std::string("nothing") : std::to_string(area_of(be)) + "/" + std::to_string(be.poly());
            }
            if (!shared) bad.push_back(side + " -> " + std::to_string(q) + ": no shared side");
            else if (!back) bad.push_back(side + " -> " + std::to_string(q) + ": its side links to " + other);
        }
    }
    return bad;
}

NavCell NavCell::of(float wx, float wy) { return {(int)std::floor((wx + 6000.0f) / 150.0f), (int)std::floor((wy + 6000.0f) / 150.0f)}; }
std::string NavCell::file_name() const { return "navmesh[" + std::to_string(x * 3) + "][" + std::to_string(y * 3) + "].ynv"; }
Vec3 NavCell::min() const { return {-6000.0f + 150.0f * x, -6000.0f + 150.0f * y, 0}; }
Vec3 NavCell::max() const { return {-6000.0f + 150.0f * (x + 1), -6000.0f + 150.0f * (y + 1), 0}; }

std::vector<std::string> cross_cell_changes(const NavMesh& before, const NavMesh& after, size_t max) {
    auto links = [](const NavMesh& n) {
        std::set<std::array<uint32_t, 3>> s;
        for (size_t p = 0; p < n.polys.size(); p++)
            for (uint32_t k = 0; k < n.polys[p].vertex_count(); k++) {
                const NavEdgePart& e = n.edges[n.polys[p].first_index + k].a;
                if (e.open() || e.area_index() >= n.adjacent_areas.size()) continue;
                const uint32_t area = n.adjacent_areas[e.area_index()];
                if (area != n.area_id) s.insert({(uint32_t)p, area, e.poly()});
            }
        return s;
    };
    const auto a = links(before), b = links(after);
    std::vector<std::string> out;
    auto say = [&](const char* what, const std::array<uint32_t, 3>& t) {
        if (out.size() < max) out.push_back(std::string(what) + ": polygon " + std::to_string(t[0]) + " -> area " + std::to_string(t[1]) + " polygon " + std::to_string(t[2]));
    };
    for (auto& t : a) if (!b.count(t)) say("lost", t);
    for (auto& t : b) if (!a.count(t)) say("new", t);
    return out;
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
