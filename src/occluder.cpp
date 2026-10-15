#include "ragekit/occluder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "ragekit/error.h"
#include "ragekit/hash.h"

namespace rk {

namespace {

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float axis_of(const Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

/// The radius of the box along the axis `n` (not necessarily unit length).
float radius_on(const OrientedBox& b, const Vec3& n) {
    return std::fabs(dot(b.axis[0], n)) * b.half.x + std::fabs(dot(b.axis[1], n)) * b.half.y + std::fabs(dot(b.axis[2], n)) * b.half.z;
}

template <class T> T get(const Bytes& b, size_t at) { T v{}; std::memcpy(&v, b.data() + at, sizeof v); return v; }
template <class T> void put(Bytes& b, size_t at, T v) { std::memcpy(b.data() + at, &v, sizeof v); }

constexpr size_t kOccludeModelSize = 64;
constexpr size_t kDataOffset = 40;   // the DataBlockPointer of the vertices and indices

} // namespace

std::array<Vec3, 8> OrientedBox::corners() const {
    std::array<Vec3, 8> out;
    for (int k = 0; k < 8; k++) {
        const float sx = (k & 1) ? 1.f : -1.f, sy = (k & 2) ? 1.f : -1.f, sz = (k & 4) ? 1.f : -1.f;
        out[k] = centre + axis[0] * (sx * half.x) + axis[1] * (sy * half.y) + axis[2] * (sz * half.z);
    }
    return out;
}

void OrientedBox::bounds(Vec3& mn, Vec3& mx) const {
    mn = {INFINITY, INFINITY, INFINITY};
    mx = {-INFINITY, -INFINITY, -INFINITY};
    for (const Vec3& c : corners()) {
        mn = {std::min(mn.x, c.x), std::min(mn.y, c.y), std::min(mn.z, c.z)};
        mx = {std::max(mx.x, c.x), std::max(mx.y, c.y), std::max(mx.z, c.z)};
    }
}

bool boxes_overlap(const OrientedBox& a, const OrientedBox& b, float margin) {
    const Vec3 t = b.centre - a.centre;
    auto separated = [&](const Vec3& n) {
        const float len = n.length();
        if (len < 1e-6f) return false;   // parallel edges: no information
        const Vec3 u = n * (1.f / len);
        return std::fabs(dot(t, u)) > radius_on(a, u) + radius_on(b, u) + margin;
    };
    for (int i = 0; i < 3; i++)
        if (separated(a.axis[i]) || separated(b.axis[i])) return false;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (separated(cross(a.axis[i], b.axis[j]))) return false;
    return true;
}

bool triangle_overlaps_box(const Vec3& a, const Vec3& b, const Vec3& c, const OrientedBox& box) {
    // the triangle in the box's frame, the box centred on the origin
    auto local = [&](const Vec3& p) {
        const Vec3 d = p - box.centre;
        return Vec3{dot(d, box.axis[0]), dot(d, box.axis[1]), dot(d, box.axis[2])};
    };
    const Vec3 v[3] = {local(a), local(b), local(c)};
    const Vec3 e[3] = {v[1] - v[0], v[2] - v[1], v[0] - v[2]};
    const Vec3 h = box.half;
    // the three box axes
    for (int i = 0; i < 3; i++) {
        const float lo = std::min({axis_of(v[0], i), axis_of(v[1], i), axis_of(v[2], i)});
        const float hi = std::max({axis_of(v[0], i), axis_of(v[1], i), axis_of(v[2], i)});
        if (lo > axis_of(h, i) || hi < -axis_of(h, i)) return false;
    }
    // the triangle's plane
    const Vec3 n = cross(e[0], e[1]);
    const float r = std::fabs(n.x) * h.x + std::fabs(n.y) * h.y + std::fabs(n.z) * h.z;
    if (std::fabs(dot(n, v[0])) > r) return false;
    // the nine edge x axis products
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) {
            Vec3 unit{0, 0, 0};
            (k == 0 ? unit.x : k == 1 ? unit.y : unit.z) = 1;
            const Vec3 ax = cross(e[i], unit);
            if (ax.length() < 1e-9f) continue;
            const float p0 = dot(ax, v[0]), p1 = dot(ax, v[1]), p2 = dot(ax, v[2]);
            const float rad = std::fabs(ax.x) * h.x + std::fabs(ax.y) * h.y + std::fabs(ax.z) * h.z;
            if (std::min({p0, p1, p2}) > rad || std::max({p0, p1, p2}) < -rad) return false;
        }
    }
    return true;
}

OrientedBox OccluderBox::box() const {
    OrientedBox o;
    o.centre = centre;
    o.half = size * 0.5f;
    o.axis[0] = {dir_x, dir_y, 0};
    o.axis[1] = {-dir_y, dir_x, 0};
    o.axis[2] = {0, 0, 1};
    return o;
}

OccluderBox decode_box_occluder(const BoxOccluder& b) {
    OccluderBox o;
    o.centre = Vec3{(float)b.center_x, (float)b.center_y, (float)b.center_z} * 0.25f;
    o.size = Vec3{(float)b.length, (float)b.width, (float)b.height} * 0.25f;
    // the length runs along (iSinZ, iCosZ): measured against 592 buildings of the same size (6 fit the other way round)
    const float c = (float)b.cos_z, s = (float)b.sin_z;
    const float len = std::sqrt(c * c + s * s);
    if (len > 0) { o.dir_x = s / len; o.dir_y = c / len; }
    return o;
}

BoxOccluder encode_box_occluder(const OccluderBox& o) {
    BoxOccluder b;
    auto q = [](float v) { return (int16_t)std::lround(v); };
    b.center_x = q(o.centre.x * 4.f); b.center_y = q(o.centre.y * 4.f); b.center_z = q(o.centre.z * 4.f);
    b.length = q(o.size.x * 4.f); b.width = q(o.size.y * 4.f); b.height = q(o.size.z * 4.f);
    b.sin_z = q(o.dir_x * 16384.f);   // the game's files hold the pair scaled to 16384
    b.cos_z = q(o.dir_y * 16384.f);
    return b;
}

OccludeMesh read_occlude_model(const MetaObject& o) {
    if (o.type != joaat("OccludeModel") || o.data.size() < kOccludeModelSize) throw Error("occlude model: not an OccludeModel object");
    OccludeMesh m;
    m.bmin = {get<float>(o.data, 0), get<float>(o.data, 4), get<float>(o.data, 8)};
    m.bmax = {get<float>(o.data, 16), get<float>(o.data, 20), get<float>(o.data, 24)};
    const uint32_t size = get<uint32_t>(o.data, 32);
    const uint32_t verts_bytes = get<uint16_t>(o.data, 48);
    m.flags = get<uint32_t>(o.data, 52);
    const Bytes* block = nullptr;
    for (const MetaLink& l : o.links)
        if (l.kind == MetaLink::Kind::DataBlock && l.offset == kDataOffset) block = &l.raw;
    if (!block) { if (size) throw Error("occlude model: no vertex data"); return m; }
    if (size > block->size() || verts_bytes > size || verts_bytes % 12) throw Error("occlude model: the data block is shorter than its size");
    m.vertices.resize(verts_bytes / 12);
    for (size_t i = 0; i < m.vertices.size(); i++)
        m.vertices[i] = {get<float>(*block, 12 * i), get<float>(*block, 12 * i + 4), get<float>(*block, 12 * i + 8)};
    m.indices.assign(block->begin() + verts_bytes, block->begin() + size);
    m.indices.resize(m.indices.size() / 3 * 3);
    return m;
}

MetaObject update_occlude_model(const MetaObject& original, const OccludeMesh& m) {
    if (m.vertices.size() > 256) throw Error("occlude model: more than 256 vertices");
    for (uint8_t i : m.indices)
        if (i >= m.vertices.size()) throw Error("occlude model: an index names no vertex");
    MetaObject o = original;
    if (o.type != joaat("OccludeModel") || o.data.size() < kOccludeModelSize) {
        o = MetaObject{};
        o.type = joaat("OccludeModel");
        o.data.assign(kOccludeModelSize, 0);
    }
    Vec3 mn{INFINITY, INFINITY, INFINITY}, mx{-INFINITY, -INFINITY, -INFINITY};
    for (const Vec3& v : m.vertices) {
        mn = {std::min(mn.x, v.x), std::min(mn.y, v.y), std::min(mn.z, v.z)};
        mx = {std::max(mx.x, v.x), std::max(mx.y, v.y), std::max(mx.z, v.z)};
    }
    if (m.vertices.empty()) mn = mx = {0, 0, 0};
    Bytes block(m.vertices.size() * 12 + m.indices.size());
    for (size_t i = 0; i < m.vertices.size(); i++) {
        put(block, 12 * i, m.vertices[i].x); put(block, 12 * i + 4, m.vertices[i].y); put(block, 12 * i + 8, m.vertices[i].z);
    }
    std::copy(m.indices.begin(), m.indices.end(), block.begin() + (long)(m.vertices.size() * 12));
    put(o.data, 0, mn.x); put(o.data, 4, mn.y); put(o.data, 8, mn.z);
    put(o.data, 16, mx.x); put(o.data, 20, mx.y); put(o.data, 24, mx.z);
    put(o.data, 12, mn.x); put(o.data, 28, mx.x);   // the two "unused" floats of the game's files hold bmin.x and bmax.x (all 1267 models)
    put(o.data, 32, (uint32_t)block.size());
    put(o.data, 48, (uint16_t)(m.vertices.size() * 12));
    put(o.data, 50, (uint16_t)(m.triangles() | 0x8000));
    put(o.data, 52, m.flags);
    bool found = false;
    for (MetaLink& l : o.links)
        if (l.kind == MetaLink::Kind::DataBlock && l.offset == kDataOffset) { l.raw = block; found = true; }
    if (!found) {
        MetaLink l;
        l.kind = MetaLink::Kind::DataBlock;
        l.offset = kDataOffset;
        l.block_type = 17;   // UnsignedByte, as the game's files and CodeWalker name the block
        l.raw = block;
        o.links.push_back(std::move(l));
    }
    return o;
}

MetaObject make_occlude_model(const OccludeMesh& m) { return update_occlude_model(MetaObject{}, m); }

namespace {

bool hits_any(const OrientedBox& b, const std::vector<OrientedBox>& volumes, float margin) {
    for (const OrientedBox& v : volumes)
        if (boxes_overlap(b, v, margin)) return true;
    return false;
}

OrientedBox widened(OrientedBox v, float margin) {
    v.half = v.half + Vec3{margin, margin, margin};
    return v;
}

bool triangle_hits_any(const Vec3& a, const Vec3& b, const Vec3& c, const std::vector<OrientedBox>& volumes, float margin) {
    for (const OrientedBox& v : volumes)
        if (triangle_overlaps_box(a, b, c, widened(v, margin))) return true;
    return false;
}

} // namespace

OccluderHits find_occluders_in(const MapData& map, const OrientedBox& box, float margin) {
    OccluderHits out;
    const std::vector<OrientedBox> v{box};
    for (size_t i = 0; i < map.box_occluders.size(); i++)
        if (hits_any(decode_box_occluder(map.box_occluders[i]).box(), v, margin)) out.boxes.push_back((uint32_t)i);
    for (size_t i = 0; i < map.occlude_models.size(); i++) {
        OccludeMesh m;
        try { m = read_occlude_model(map.occlude_models[i]); } catch (const Error&) { continue; }
        for (size_t t = 0; t < m.triangles(); t++)
            if (m.indices[3 * t] < m.vertices.size() && m.indices[3 * t + 1] < m.vertices.size() && m.indices[3 * t + 2] < m.vertices.size() &&
                triangle_hits_any(m.vertices[m.indices[3 * t]], m.vertices[m.indices[3 * t + 1]], m.vertices[m.indices[3 * t + 2]], v, margin)) {
                out.models.push_back((uint32_t)i);
                break;
            }
    }
    return out;
}

std::vector<int32_t> remove_entities(MapData& map, std::vector<uint32_t> indices) {
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    std::vector<int32_t> remap(map.entities.size(), -1);
    std::vector<Entity> keep;
    keep.reserve(map.entities.size());
    size_t next = 0;
    for (size_t i = 0; i < map.entities.size(); i++) {
        while (next < indices.size() && indices[next] < i) next++;
        if (next < indices.size() && indices[next] == i) continue;
        remap[i] = (int32_t)keep.size();
        keep.push_back(std::move(map.entities[i]));
    }
    map.entities = std::move(keep);
    return remap;
}

size_t remap_parent_indices(MapData& child, const std::vector<int32_t>& remap) {
    size_t changed = 0;
    for (Entity& e : child.entities) {
        if (e.parent_index < 0) continue;
        const int32_t to = (size_t)e.parent_index < remap.size() ? remap[(size_t)e.parent_index] : -1;
        if (to != e.parent_index) { e.parent_index = to; changed++; }
    }
    return changed;
}

} // namespace rk
