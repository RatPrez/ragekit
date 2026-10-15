// ragekit - occluders of a map file (.ymap) and removing things from one.
//
// A CMapData holds two kinds of occluder (software occlusion: what lies behind them, seen from the camera, is not drawn):
//   * box occluders (`MapData::box_occluders`, 16 bytes each): a box rotated about z, all in world space, quantised to a
//     quarter of a metre (centre and size) and 1/16384 (the rotation's sine and cosine; the box's length runs along (sin, cos));
//   * occlude models (`MapData::occlude_models`, 64 bytes + a data block): a small triangle mesh, at most 256 vertices
//     (12 bytes each, world space) followed by the triangle corners as bytes.
// Occlusion ymaps are ordinary ymaps with content flag 32 (`MapData::content_flags`); a ymap can hold both kinds beside
// entities. Both are conservative solids: they only hide what cannot be seen anyway, so removing one never hides anything
// that was visible, it only lets things behind it be drawn (and drawn more often).
//
// Also here: the box tests the walk-in export needs (an oriented box against another box, a triangle against a box),
// and `remove_entities`, which drops entities from a ymap keeping the LOD parent links of the rest intact.
#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include "map.h"

namespace rk {

/// An oriented box: centre, half sizes along three orthonormal axes.
struct OrientedBox {
    Vec3 centre;
    Vec3 half;
    Vec3 axis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    /// The eight corners (bit 0 = +axis0, bit 1 = +axis1, bit 2 = +axis2).
    std::array<Vec3, 8> corners() const;
    /// The smallest axis aligned box around it.
    void bounds(Vec3& mn, Vec3& mx) const;
};

/// True when the interiors of the two boxes overlap (separating axis test, `margin` metres of slack on every axis).
bool boxes_overlap(const OrientedBox& a, const OrientedBox& b, float margin = 0);
/// True when the triangle and the box overlap (touching counts).
bool triangle_overlaps_box(const Vec3& a, const Vec3& b, const Vec3& c, const OrientedBox& box);

/// A box occluder in metres. (dir_x, dir_y) is the unit vector the box's length runs along in the xy plane, the width runs along
/// its left. The file stores it as (iSinZ, iCosZ) scaled to 16384, so "cos" is the y component (a heading from north, clockwise).
struct OccluderBox {
    Vec3  centre;
    Vec3  size;                      // length, width, height (full)
    float dir_x = 1, dir_y = 0;
    OrientedBox box() const;
};
OccluderBox decode_box_occluder(const BoxOccluder& b);
/// Quantises (centre and size to 0.25 m, the direction to 1/16384).
BoxOccluder encode_box_occluder(const OccluderBox& b);

/// An occlude model decoded.
struct OccludeMesh {
    Vec3     bmin, bmax;             // as stored (the vertices' extent)
    uint32_t flags = 0;
    std::vector<Vec3>    vertices;
    std::vector<uint8_t> indices;    // three per triangle
    size_t triangles() const { return indices.size() / 3; }
};
/// Reads an entry of `MapData::occlude_models` (throws rk::Error when the object is not one or its data block is short).
OccludeMesh read_occlude_model(const MetaObject& o);
/// A model object holding the mesh (numTris carries the 0x8000 bit the game's files have; bounds and sizes are recomputed).
/// The mesh must have at most 256 vertices and every index must name one.
MetaObject make_occlude_model(const OccludeMesh& m);
/// The same for a model that exists: its padding and unused fields are kept, the mesh, bounds and sizes replaced.
MetaObject update_occlude_model(const MetaObject& original, const OccludeMesh& m);

/// The box occluders and occlude models of `map` that overlap `box`, as indices into `box_occluders` / `occlude_models`
/// (a model counts when any triangle overlaps).
struct OccluderHits { std::vector<uint32_t> boxes, models; };
OccluderHits find_occluders_in(const MapData& map, const OrientedBox& box, float margin = 0);

/// Removes entities (indices into `map.entities`); everything else in the map stays as it is (extents and flags too: a
/// streaming box that is too large is harmless). The LOD links of OTHER ymaps point into this list by index (`parent_index` of
/// the entities of the ymaps that name this one as their parent), so the result says where every entity went: old index -> new
/// index, -1 for the removed ones. Apply it to those ymaps with `remap_parent_indices`.
std::vector<int32_t> remove_entities(MapData& map, std::vector<uint32_t> indices);
/// Renumbers the `parent_index` of the entities of `child` (a ymap whose parent is the map `remap` came from); an entity whose
/// parent was removed becomes parentless. Returns how many entities changed.
size_t remap_parent_indices(MapData& child, const std::vector<int32_t>& remap);

} // namespace rk
