// ragekit - navmeshes (.ynv): the surfaces peds walk on.
//
// The map is a grid of 100 x 100 cells of 150 m (x and y from -6000 to 9000), one file per cell,
// navmesh[X*3][Y*3].ynv, with area id X + Y*100. A cell holds polygons (3 to 13 vertices, counter-clockwise), one edge
// record per polygon side saying what lies across it, cover points kept in a quadtree of sectors, special links
// ("portals": climbs, drops, ladders) and per-polygon flags. Neighbouring polygons are linked by index (an index into
// the cell's adjacent area list and a polygon number), inside the cell and across its borders; renumbering polygons
// breaks every link that points at them. Vertices and points are 16-bit fractions of the cell's bounding box.
//
// NavMesh keeps every stored value as the file holds it (polygon boxes, portal link offsets, sector leaf lists, the
// adjacent area order), so a read -> write gives the game's bytes back. Layout from CodeWalker's Nav.cs; the meaning of
// the edge and flag bits from tests on the game's own cells, checked in game.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include "types.h"

namespace rk {

/// A 16-bit position: a fraction (0..65535) of the cell's bounding box on each axis.
struct NavVertex { uint16_t x = 0, y = 0, z = 0; };

/// One half of an edge record.
struct NavEdgePart {
    uint32_t value = 0;
    uint32_t area_index() const { return value & 0x1F; }        // into NavMesh::adjacent_areas
    uint32_t poly() const { return (value >> 5) & 0x3FFF; }     // 0x3FFF: none (a wall, or the edge of the mesh)
    uint32_t link_kind() const { return (value >> 19) & 0x3; }  // 0 a link, 1 an open edge, 2 a drop / climb to a polygon sharing no edge
    uint32_t space() const { return (value >> 21) & 0x7FF; }    // links: free space at the edge in 0.25 m steps << 1, bit 0 a height step
    bool open() const { return poly() == 0x3FFF; }
    static NavEdgePart make(uint32_t area_index, uint32_t poly, uint32_t kind, uint32_t space) {
        return {(area_index & 0x1F) | (poly & 0x3FFF) << 5 | (kind & 3) << 19 | (space & 0x7FF) << 21};
    }
};
struct NavEdge { NavEdgePart a, b; };   // b repeats a's area and polygon on ordinary links

/// A box in quarter metres (signed 16-bit), as polygons and sectors store it.
struct NavBox { int16_t min_x = 0, max_x = 0, min_y = 0, max_y = 0, min_z = 0, max_z = 0; };

struct NavPoly {
    uint16_t flags0 = 0;         // byte 0: flags (pavement, road, interior, water ...)
    uint16_t index_flags = 0;    // vertex count << 5
    uint16_t first_index = 0;    // into NavMesh::indices and edges
    uint16_t area_id = 0;        // the cell's own
    uint32_t unused[4] = {0, 0, 0, 0};
    NavBox box;                  // computed by the game from its float positions before rounding: kept, not recomputed
    uint32_t flags1 = 0;
    uint32_t flags2 = 0;         // bytes 0-1: the centre inside the box (0-255 per axis); bits 16-23 a direction byte
    uint32_t part_flags = 0;     // part id (bits 4-11), portal link count (12-14) and first link (15-31)

    uint32_t vertex_count() const { return index_flags >> 5; }
    void set_vertex_count(uint32_t n) { index_flags = (uint16_t)((index_flags & 31) | (n & 0x7FF) << 5); }
    uint32_t portal_link_count() const { return (part_flags >> 12) & 7; }
    uint32_t portal_link_first() const { return part_flags >> 15; }
};

/// A cover point: position, direction (256 steps) and type.
struct NavPoint { uint16_t x = 0, y = 0, z = 0; uint8_t angle = 0, type = 0; };

/// A special link between two polygons: a climb, a drop or a ladder.
struct NavPortal {
    uint8_t type = 0, angle = 0;
    uint16_t flags = 0;
    NavVertex from, to;
    uint16_t poly_from1 = 0, poly_from2 = 0, poly_to1 = 0, poly_to2 = 0;
    uint32_t area_flags = 0;     // area from (bits 0-13), area to (14-27)
};

/// A node of the sector quadtree; the leaves list their polygons and hold the cover points.
struct NavSector {
    Vec4 min, max;               // W as stored (NaN)
    NavBox box;
    std::array<std::unique_ptr<NavSector>, 4> children;
    bool has_data = false;
    uint32_t points_start = 0;   // index of the first point across the leaves
    std::vector<uint16_t> polys;
    std::vector<NavPoint> points;

    NavSector() = default;
    NavSector(const NavSector& o);
    NavSector& operator=(const NavSector& o);
    NavSector(NavSector&&) = default;
    NavSector& operator=(NavSector&&) = default;
};

struct NavMesh {
    uint32_t content_flags = 1;  // 1 polygons, 2 portals, 4 vehicle, 8 (most map cells), 16 (cells rebuilt by a DLC)
    uint32_t version = 0x00010011;
    uint32_t build = 0x85CB3561; // 0x85CB3561 on the base game's cells, 0 vehicle meshes, another value per DLC export
    std::array<float, 16> transform{1, 0, 0, NAN, 0, 1, 0, NAN, 0, 0, 1, NAN, 0, 0, 0, NAN};
    Vec3 size;                   // the box's size (the sector tree's root holds the box itself)
    uint32_t size_w = 0x7F800001;
    uint32_t area_id = 0;        // X + Y*100
    std::vector<uint32_t> adjacent_areas;   // at most 32; edges name their area by index into this list
    std::vector<NavVertex> vertices;
    std::vector<uint16_t> indices;          // polygon corners, polygon after polygon
    std::vector<NavEdge> edges;             // one per index: the side from that corner to the next
    std::vector<NavPoly> polys;
    std::optional<NavSector> sectors;
    std::vector<NavPortal> portals;
    std::vector<uint16_t> portal_links;

    Vec3 box_min() const;
    Vec3 box_max() const;
    /// A stored position in world coordinates, and back (rounded to the 16-bit grid).
    Vec3 position(const NavVertex& v) const;
    NavVertex quantize(const Vec3& p) const;
    /// The corners of a polygon, in world coordinates.
    std::vector<Vec3> poly_vertices(size_t poly) const;
    /// The cover points of every leaf, in order.
    std::vector<NavPoint> points() const;
    /// The header's byte total: 6 per vertex, 10 per index, 48 per polygon, 28 per portal, 8 per point, 2 per leaf
    /// polygon id, 120 per sector (96 + its 32 byte data block for leaves), as the game's files have it.
    uint32_t total_bytes() const;

    // ---------------------------------------------------------------- editing

    /// A polygon as an edit sees it.
    struct PolyShape {
        NavPoly poly;                 // flags and the rest as stored (vertex count and first index are set on rebuild)
        std::vector<Vec3> corners;    // world space, counter-clockwise seen from above
        std::vector<NavEdge> edges;   // one per corner: the side from it to the next
        /// The polygon of this cell the shape is, unmoved (shape() sets it): its stored corners, box, centre and sector
        /// leaves are kept. -1 for a new or reshaped polygon: those are worked out from `corners`.
        int32_t source = -1;
    };
    PolyShape shape(size_t poly) const;
    std::vector<PolyShape> shapes() const;
    /// Replaces the polygon list (a polygon's number is its place in `polys`) and rebuilds vertices, indices, edges
    /// and the sector leaves' polygon lists. A reshaped polygon (source -1) gets its box (the corners in quarter metres,
    /// towards zero, as the game computes it), its centre bytes, its portal link offset and every leaf its box reaches.
    /// set_polys(shapes()) gives the cell back unchanged. Portals and the neighbouring cells name polygons by number:
    /// keep numbers stable where something links to them.
    void set_polys(const std::vector<PolyShape>& polys);

    /// The cover points of every leaf can be edited in place; this renumbers the leaves' first point after.
    void renumber_points();

    /// Links that do not hold, from or to the given polygons (all of them when empty): an in-cell link must name a
    /// polygon sharing that side whose own side links back. Drop / climb links (kind 2, or the height step bit) and
    /// links into other cells are not checked.
    std::vector<std::string> check_links(std::span<const uint32_t> polys = {}) const;
};

/// The 150 m navmesh cells: index x, y = floor((world - (-6000)) / 150), area id x + y*100.
struct NavCell {
    int x = 0, y = 0;
    static NavCell of(float wx, float wy);
    static NavCell of_area(uint32_t area_id) { return {(int)(area_id % 100), (int)(area_id / 100)}; }
    uint32_t area_id() const { return (uint32_t)(x + y * 100); }
    std::string file_name() const;   // navmesh[X*3][Y*3].ynv
    Vec3 min() const;                // z 0
    Vec3 max() const;
};

/// The links into other cells that differ between two copies of a cell (polygon, area, polygon there): the
/// neighbours name this cell's border polygons by number, so an edit must leave these as they were.
std::vector<std::string> cross_cell_changes(const NavMesh& before, const NavMesh& after, size_t max = 20);

/// .ynv (version 2 in both editions).
NavMesh read_ynv(std::span<const uint8_t> file);
Bytes write_ynv(const NavMesh& nav);

} // namespace rk
