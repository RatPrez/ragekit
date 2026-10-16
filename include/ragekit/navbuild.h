// ragekit - rebuilding part of a navmesh cell from collision.
//
// patch_navmesh replaces the polygons of one area of a cell with polygons made from the walkable ground under them,
// stitched to the polygons kept around them. The new polygons take the replaced polygons' numbers (extra ones go on
// the end), so nothing else in the cell is renumbered and the neighbouring cells' links stay valid: the patched cell
// can be shipped on its own.
//
// The method, tuned against the game's own mesh on the game's collision (on the Mission Row police station's front the
// rebuilt area covers the same ground as the game's mesh at 91%, heights within 0.19 m):
//  1. sample every surface under a 0.25 m grid; a floor is a surface no steeper than 45 degrees with 1.8 m clear
//     above it, not inside a solid;
//  2. flood the ground from a seed (steps of at most 0.3 m), keep 0.5 m from walls and drops, and block 0.4 m around
//     anything the grid misses that rises more than 0.5 m above the ground beside it (rails, posts) and the props
//     given (0.25 m);
//  3. smooth heights over about 1 m (a flight of box steps becomes one ramp, as the game models stairs), split the
//     ground into regions lying within 0.1 m of a plane, merge small ones;
//  4. outline the regions together (shared borders stay identical), clip to the area, triangulate (constrained
//     Delaunay) and merge into convex polygons of at most 8 corners;
//  5. split sides where a corner of the other side lands on them, link sides whose ends meet at the same height
//     (2 cm grid), drop slivers under 0.03 m2; copy flags from the replaced polygon underneath, compute the free
//     space at every link, drop cover points left standing in the open.
// Drop-down and climb links and new cover points are not generated.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "bounds.h"
#include "navmesh.h"
#include "types.h"

namespace rk {

/// The collision a patch is built from, in world space.
struct NavCollision {
    struct Triangle {
        Vec3 v[3];          // counter-clockwise seen from outside
        bool solid = false; // part of a closed primitive (box, sphere, capsule, cylinder): the space inside is solid
    };
    std::vector<Triangle> triangles;
    /// Things cut into the mesh that the collision does not hold (bollards, placed props): circles from z to z + height.
    struct Post { float x = 0, y = 0, z = 0, radius = 0.12f, height = 1.0f; };
    std::vector<Post> posts;

    /// Adds the surfaces of a static collision file that peds collide with. Composite children flagged for anything but
    /// map collision (foliage, cover-only) are left out. With a clip box, only what reaches it.
    void add_bound(const Bound& b, const AABB* clip = nullptr);
};

struct NavPatchOptions {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // the area: polygons whose centre is inside are rebuilt
    float z_lo = -1000, z_hi = 1000;        // only polygons within these heights (keeps roofs and tunnels out)
    std::optional<Vec3> seed;               // a point on the ground to flood from (default: the replaced polygons' centres)

    float cell = 0.25f;         // sample spacing
    float step = 0.3f;          // largest height step between neighbouring samples
    float head = 1.8f;          // headroom
    int clear_cells = 2;        // clearance from walls and drops, in samples
    float knee = 0.5f;          // a thin thing blocks when it rises this far above the ground beside it
    float rail_clear = 0.4f;    // clearance around thin things
    float post_clear = 0.25f;   // clearance around posts
    float plane_tolerance = 0.1f;
    float simplify = 0.2f;      // outline simplification
    int max_corners = 8;
};

struct NavPatchResult {
    uint32_t removed = 0, created = 0, kept_changed = 0, links = 0, open_sides = 0, regions = 0, blocked_samples = 0, points_dropped = 0;
    float worst_height = 0;               // largest distance between a new polygon and the ground under it
    std::vector<uint32_t> written;        // polygon numbers written (new and stitched)
    std::vector<std::string> notes;
};

/// The box the collision must cover for a patch: the area, every polygon it replaces and a margin.
AABB nav_patch_extent(const NavMesh& nav, const NavPatchOptions& opt);
/// Rebuilds the area (see the top of this file). Throws rk::Error when the area reaches the cell's border (a polygon
/// in it links into the next cell), holds no polygons, or no ground is found to start from.
NavPatchResult patch_navmesh(NavMesh& nav, const NavPatchOptions& opt, const NavCollision& collision);

} // namespace rk
