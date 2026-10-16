// ragekit - collision bounds (.ybn, and the bounds embedded in drawables and fragments).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "types.h"

namespace rk {

enum class BoundType : uint8_t {
    Sphere = 0, Capsule = 1, Box = 3, Geometry = 4, GeometryBVH = 8, Composite = 10, Disc = 12, Cylinder = 13, Cloth = 15
};
const char* bound_type_name(BoundType t);

/// A collision material: the game material index plus per-polygon flags.
struct BoundMaterial {
    uint32_t data1 = 0, data2 = 0;
    uint8_t type() const { return (uint8_t)(data1 & 0xFF); }
    void set_type(uint8_t t) { data1 = (data1 & 0xFFFFFF00u) | t; }
    uint8_t procedural_id() const { return (uint8_t)((data1 >> 8) & 0xFF); }
    uint8_t room_id() const { return (uint8_t)((data1 >> 16) & 0x1F); }
    void set_room_id(uint8_t r) { data1 = (data1 & 0xFFE0FFFFu) | ((uint32_t)(r & 0x1F) << 16); }
    uint8_t ped_density() const { return (uint8_t)((data1 >> 21) & 0x7); }
    uint16_t flags() const { return (uint16_t)(((data1 >> 24) & 0xFF) | ((data2 & 0xFF) << 8)); }
    void set_flags(uint16_t f) { data1 = (data1 & 0x00FFFFFFu) | ((uint32_t)(f & 0xFF) << 24); data2 = (data2 & 0xFFFFFF00u) | ((f >> 8) & 0xFF); }
    uint8_t colour_index() const { return (uint8_t)((data2 >> 8) & 0xFF); }
    bool operator==(const BoundMaterial& o) const { return data1 == o.data1 && data2 == o.data2; }
};

/// Material flag bits (BoundMaterial::flags).
enum BoundMaterialFlags : uint16_t {
    kMatStairs = 1, kMatNotClimbable = 2, kMatSeeThrough = 4, kMatShootThrough = 8, kMatNotCover = 16,
    kMatWalkablePath = 32, kMatNoCamCollision = 64, kMatShootThroughFx = 128, kMatNoDecal = 256, kMatNoNavmesh = 512,
    kMatNoRagdoll = 1024, kMatVehicleWheel = 2048, kMatNoPtfx = 4096, kMatTooSteepForPlayer = 8192,
    kMatNoNetworkSpawn = 16384, kMatNoCamCollisionAllowClipping = 32768
};

/// One collision primitive of a geometry bound, stored as the game stores it (16 bytes).
struct BoundPolygon {
    enum Kind : uint8_t { Triangle = 0, Sphere = 1, Capsule = 2, Box = 3, Cylinder = 4 };
    std::array<uint8_t, 16> raw{};
    Kind kind = Triangle;

    // Triangle: area, three vertex indices (15 bits, top bit is a flag), three neighbour polygon indices (0xFFFF = none)
    float area() const; void set_area(float a);
    uint16_t vertex(int i) const;      // i = 0..2 (triangle), 0..3 (box), 0..1 (capsule/cylinder), 0 (sphere)
    bool vertex_flag(int i) const;
    void set_vertex(int i, uint16_t v);   // renumbers a vertex, keeping a triangle's flag bit
    uint16_t edge(int i) const; void set_edge(int i, int32_t poly);
    // Sphere / Capsule / Cylinder
    float radius() const; void set_radius(float r);

    static BoundPolygon triangle(uint16_t a, uint16_t b, uint16_t c);
    static BoundPolygon sphere(uint16_t center, float radius);
    static BoundPolygon capsule(uint16_t a, uint16_t b, float radius);
    static BoundPolygon box(uint16_t a, uint16_t b, uint16_t c, uint16_t d);
    static BoundPolygon cylinder(uint16_t a, uint16_t b, float radius);
    int vertex_count() const { return kind == Triangle ? 3 : kind == Box ? 4 : kind == Sphere ? 1 : 2; }
};

/// Bounding volume hierarchy over polygons (geometry) or children (composite).
struct BoundBVH {
    struct Node { int16_t min[3]; int16_t max[3]; int16_t item_id; int16_t item_count; };
    struct Tree { int16_t min[3]; int16_t max[3]; int16_t node_first; int16_t node_end; };
    std::vector<Node> nodes;      // capacity entries (composites pad with empty nodes)
    uint32_t node_count = 0;      // entries in use
    Vec4 bbox_min, bbox_max, bbox_center, quantum_inverse, quantum;
    std::vector<Tree> trees;
};

/// Transform of a composite child: 3x4 matrix plus the four flag words that follow each column.
struct BoundTransform {
    Vec3 col[4]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}};
    uint32_t flags[4] = {0, 1, 1, 0};
    Mat4 matrix() const;
    static BoundTransform from(const Mat4& m);
    // xml: the file stores each column as xyz + its flag word (16 bytes per column, 64 in all); the
    // members above are the same data split into columns and flags. Read and write through these two,
    // not by copying the struct's memory.
    static BoundTransform from_file(const uint8_t* p64);
    void to_file(uint8_t* p64) const;
};

struct Bound;
using BoundPtr = std::shared_ptr<Bound>;

struct BoundChild {
    BoundPtr bound;                 // null entries occur in fragments
    BoundTransform transform;
    AABB bbox;
    uint32_t flags1 = 0, flags2 = 0; // composite flags (EBoundCompositeFlags pairs: what it collides with)
};

struct Bound {
    BoundType type = BoundType::Geometry;
    // phBound (112 bytes)
    float sphere_radius = 0;
    Vec3 box_max, box_min, box_center, sphere_center;
    float margin = 0.04f;
    uint32_t unk3C = 1;              // phBound::m_RefCount: 1, or 2 in fragments (where the bound is shared)
    uint8_t material_index = 0, procedural_id = 0, room_ped_density = 0, unk_flags = 0;
    uint8_t poly_flags = 0, material_colour_index = 0;
    uint16_t unk5E = 0;              // the upper half of the 64-bit material id (m_MaterialID1)
    Vec3 inertia;
    float volume = 0;
    float unk70 = 0;                 // capsule, disc, cylinder (128 bytes): a capsule's half height (phBoundCapsule::m_CapsuleHalfHeight)

    // Geometry / GeometryBVH
    struct Geometry {
        uint16_t unk80 = 0, unk82 = 0;   // phBoundPolyhedron: two unused bytes; m_NumPerVertexAttributes (1 with vertex colours)
        Vec3 quantum;                 // vertex quantisation step (0 = compute on write)
        float unk9C = 0, unkAC = 0;      // the W lanes of the quantisation factor and of the centre
        Vec3 center;                  // vertices are relative to this
        std::vector<Vec3> vertices, vertices_shrunk;
        std::vector<BoundPolygon> polygons;
        std::vector<uint32_t> vertex_colours;     // RGBA bytes packed, optional
        std::vector<BoundMaterial> materials;
        std::vector<uint32_t> material_colours;
        std::vector<uint8_t> polygon_materials;   // material index per polygon
        bool has_octants = false;
        std::array<std::vector<uint32_t>, 8> octants;
        std::optional<BoundBVH> bvh;              // GeometryBVH only
        uint16_t unk140 = 0xFFFF;                 // GeometryBVH only
    };
    std::optional<Geometry> geometry;

    // Composite
    std::vector<BoundChild> children;
    bool has_child_flags = true;          // ybn composites carry the flag arrays; fragment ones do not
    bool has_child_transforms2 = false;   // a second, distinct transform array
    std::vector<BoundTransform> child_transforms2;
    std::optional<BoundBVH> composite_bvh;

    uint8_t room_id() const { return room_ped_density & 0x1F; }
    uint8_t ped_density() const { return room_ped_density >> 5; }
    void set_room_id(uint8_t r) { room_ped_density = (uint8_t)((room_ped_density & 0xE0) | (r & 0x1F)); }

    /// World-space position of a geometry vertex (vertex + center).
    Vec3 vertex_position(size_t i) const;

    /// Builds a triangle-mesh bound (GeometryBVH by default) from vertices and triangles.
    static BoundPtr make_mesh(const std::vector<Vec3>& vertices, const std::vector<uint32_t>& triangles,
                              const BoundMaterial& material, bool bvh = true);
    /// Builds a composite around children (recomputes boxes, spheres and the composite BVH).
    static BoundPtr make_composite(std::vector<BoundChild> children);
    /// Recomputes everything derived: boxes, sphere, quantum, edges, areas, materials table,
    /// shrunk vertices, octants, BVH. Call after editing a geometry.
    void finalize();
};

/// .ybn files: a single bound (usually a composite).
BoundPtr read_ybn(std::span<const uint8_t> file);
Bytes write_ybn(const Bound& bound);

/// A collision triangle in the space the outermost bound sits in, with the material of its polygon.
struct BoundTriangle {
    Vec3 v[3];
    BoundMaterial material;
};
/// Every surface of a bound as triangles: composite children through their transforms, triangle polygons as they
/// are, box polygons (four diagonal corners) as 12 triangles, sphere / capsule / cylinder polygons and whole primitive
/// bounds as coarse meshes with `segments` around. With a clip box, only triangles whose box reaches it.
std::vector<BoundTriangle> bound_triangles(const Bound& b, int segments = 8, const AABB* clip = nullptr);
/// One polygon of a geometry bound as triangles in the geometry's own space (a triangle: itself; a box: 12; a sphere,
/// capsule or cylinder: a coarse mesh with `segments` around).
std::vector<BoundTriangle> polygon_triangles(const Bound::Geometry& g, size_t polygon, int segments = 8);

} // namespace rk
