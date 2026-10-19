// ragekit - path nodes (.ynd): the road and pavement graph vehicles and peds navigate by, one file per 512 m cell
// (paths.rpf/nodes<n>.ynd, n = y * 32 + x of the cell).
//
// A file holds the nodes of its cell (vehicle nodes first, then ped nodes), the links between them (each node owns a
// run of links, which may lead into neighbouring cells by area id), and the junctions with their height maps.
#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include "types.h"

namespace rk {

/// One node (40 bytes, as stored).
struct PathNode {
    uint32_t unused[4] = {};         // 0 in the game's files (the game's runtime data)
    uint16_t area_id = 0;            // the cell: y * 32 + x
    uint16_t node_id = 0;            // the index in its file
    uint32_t street_name = 0;        // joaat of the street's text label
    uint16_t unused14 = 0;           // 0 on all 309,416 nodes
    uint16_t link_id = 0;            // the first of its links in the file's link list
    int16_t position_x = 0, position_y = 0;   // metres x 4
    uint8_t flags0 = 0, flags1 = 0;
    int16_t position_z = 0;          // metres x 32
    uint8_t flags2 = 0;
    uint8_t link_count_flags = 0;    // link count << 3 | 3 bits of flags
    uint8_t flags3 = 0, flags4 = 0;

    Vec3 position() const { return {position_x / 4.f, position_y / 4.f, position_z / 32.f}; }
    int link_count() const { return link_count_flags >> 3; }
};
static_assert(sizeof(PathNode) == 40);

/// A link from a node to another (8 bytes).
struct PathLink {
    uint16_t area_id = 0, node_id = 0;   // the node it leads to
    uint8_t flags0 = 0, flags1 = 0, flags2 = 0;
    uint8_t length = 0;              // metres, rounded
};
static_assert(sizeof(PathLink) == 8);

/// A junction (12 bytes): its box and a height map of heightmap_dim_x x heightmap_dim_y bytes.
struct PathJunction {
    int16_t max_z = 0;               // metres x 32
    int16_t position_x = 0, position_y = 0;   // metres x 4: the height map's corner
    int16_t min_z = 0;
    uint16_t heightmap_offset = 0;   // into PathNodes::heightmaps
    uint8_t heightmap_dim_x = 0, heightmap_dim_y = 0;
};
static_assert(sizeof(PathJunction) == 12);

/// Which node a junction belongs to (8 bytes).
struct PathJunctionRef {
    uint16_t area_id = 0, node_id = 0, junction_id = 0;
    uint16_t unused6 = 0;            // 0 on all 12,273
};
static_assert(sizeof(PathJunctionRef) == 8);

struct PathNodes {
    std::vector<PathNode> nodes;     // the vehicle nodes, then the ped nodes
    uint32_t vehicle_node_count = 0, ped_node_count = 0;
    std::vector<PathLink> links;
    std::vector<PathJunction> junctions;
    Bytes heightmaps;                // every junction's height map, one byte a cell
    std::vector<PathJunctionRef> junction_refs;
    uint16_t junction_ref_count2 = 0;   // the root's second junction-reference count: the list's size in every game file
    uint32_t unk48 = 1;              // the root's u32 at 0x48: 1 in every game file
};

PathNodes read_ynd(std::span<const uint8_t> file);
Bytes write_ynd(const PathNodes& n);

}  // namespace rk
