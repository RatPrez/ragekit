// ragekit - the game world: where every map file (.ymap) places its entities and which static collision (.ybn)
// covers which area. Built on a GameIndex; answers "what stands here" for tools that place things in the world.
//
// Which files the game uses: the base map archives, minus those the mounted DLC packs invalidate and plus those they
// enable through their content change sets (content.xml; the groups of setup2.xml, group by group in pack order: the
// startup and title-update groups, then GROUP_MAP), then the newest copy of each name among the mounted archives. A
// pack archive "<device>:/%PLATFORM%/X" overlays the base "platform:/X" and goes when that path is invalidated. Checked
// against CodeWalker's world (`oracle world`, rk_world_test --oracle).
//
// open() reads the header of every .ymap the game uses,
// every MLO instance, the IPL groups of the `_manifest.ymf` files (ymaps a script must request) and the game's own
// bounds store (`*_cache_y.dat`: the extents of every static collision file; files no cache lists are read). The
// result is cached next to the GameIndex cache; a cached world index opens in milliseconds, a scan takes seconds.
// Entities themselves are not kept: a query loads the few ymaps whose extents reach the area (kept in a small cache).
//
// Rotations: a ymap stores an ordinary entity's rotation as the INVERSE of its world orientation, but an MLO instance
// (CMloInstanceDef) as the orientation itself: the game's interior proxies (gta5_cache_y.dat) fit the stored quaternion
// for 163 rotated interiors and its inverse for none (world_test). Entities inside an MLO are stored inverted again.
// WorldEntity::orientation is always the world orientation.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "types.h"
#include "map.h"
#include "gameindex.h"

namespace rk {

/// One map file the game loads.
struct WorldMap {
    uint32_t name = 0;                 // joaat_lower of the ymap name
    uint32_t parent = 0;               // the parent ymap's name (0 = none)
    int32_t  parent_map = -1;          // index of the parent in WorldIndex::maps()
    IndexedFile file;                  // the winning .ymap
    uint32_t flags = 0;                // CMapData flags: 1 script loaded, 2 has LOD
    uint32_t content_flags = 0;        // 1 HD, 2 LOD, 4 SLOD2+, 8 interior, 16 SLOD, 32 occluder, 64 physics, 128 lod lights, 256 distant lod lights, 512 critical, 1024 instance list
    Vec3     streaming_min, streaming_max;
    Vec3     entities_min, entities_max;
    uint32_t entity_count = 0;
    uint32_t mlo_instances = 0;
    uint8_t  group = 0;                // 1: listed as a CMapDataGroup (an IPL a script requests; not loaded by default)
    uint8_t  readable = 1;             // 0: the file could not be read (a handful of PSO ymaps): extents unknown
    uint16_t pad = 0;
};

/// An interior placed in the world (CMloInstanceDef of some ymap).
struct WorldMloInstance {
    uint32_t archetype = 0;            // the MLO archetype's name hash
    uint32_t map = 0;                  // index into WorldIndex::maps()
    uint32_t entity = 0;               // index of the instance in that ymap's entity list
    Vec3     position;
    Quat     orientation;              // world orientation (the file's quaternion as stored)
    Vec3     box_min, box_max;         // world AABB of the interior's rooms (the position when the archetype is unknown)
    Vec3     local_min, local_max;     // the rooms' box (limbo included) in the interior's own space; zero when unknown
    uint32_t group_id = 0, floor_id = 0, inst_flags = 0, num_exit_portals = 0;
};

/// A static collision file and the box it covers.
struct WorldBounds {
    uint32_t name = 0;                 // joaat_lower of the .ybn name
    IndexedFile file;                  // the winning .ybn
    Vec3     box_min, box_max;         // world space; an interior's collision is in the interior's own space
    uint32_t layer = 0;                // the bounds store's layer: 0x7F00 + 0 ordinary, 1 high detail (`hi@` files), 2 `ma@` files
    uint32_t interior = 0;             // the MLO archetype whose CInteriorBoundsFiles lists it (0 = map collision)
    uint8_t  cached = 1;               // 1: extents from the game's bounds store, 0: read from the file
    uint8_t  pad[3] = {};
};

/// An entity found by a query.
struct WorldEntity {
    uint32_t map = 0;                  // index into WorldIndex::maps()
    uint32_t index = 0;                // index in that ymap's entity list
    Entity   def;                      // as the file stores it
    Quat     orientation;              // world orientation
    Vec3     box_min, box_max;         // world AABB of the archetype's box, scaled; the position when the archetype is unknown
    bool     archetype_known = false;
};

/// Which entities a query returns.
struct WorldQuery {
    uint32_t lod_mask = (1u << 0) | (1u << 5);   // bit n = lod level n (0 HD, 1 LOD, 2 SLOD1, 3 SLOD2, 4 SLOD3, 5 ORPHANHD, 6 SLOD4)
    bool     groups = false;           // include ymaps of IPL groups
    bool     mlo_instances = true;     // include MLO instances
};

struct WorldIndexOptions {
    /// The single-player map: the packs' GROUP_MAP change sets (the multiplayer map: heist-era replacements of whole
    /// districts, prefixed hei_ / apa_ / lr_ ...) are not applied. GUESS: entering SP reverts them; FiveM shows the MP map.
    bool sp_map = false;
    bool rebuild = false;
    bool use_cache = true;
    std::string cache_folder;          // default: the GameIndex cache folder (%USERPROFILE%\.ragekit\index)
    int threads = 0;
    std::function<void(const std::string&)> progress;
};

struct WorldIndexInfo {
    bool from_cache = false;
    uint32_t archives_invalidated = 0; // map archives a change set turned off
    uint32_t archives_enabled = 0;     // DLC map archives a change set turned on
    double seconds = 0, scan_seconds = 0;
    std::string cache_path;
    std::vector<std::string> warnings;
};

/// World orientation of an entity (inverts the stored quaternion of everything but MLO instances).
Quat entity_orientation(const Entity& e);
/// q applied to v.
Vec3 rotate(const Quat& q, const Vec3& v);
/// World AABB of a local box under scale, orientation and position.
void transform_box(const Vec3& mn, const Vec3& mx, const Vec3& scale, const Quat& q, const Vec3& pos, Vec3& out_min, Vec3& out_max);

class WorldIndex {
public:
    static constexpr uint32_t kFormat = 3;

    /// Builds (or loads from the cache) the world index of the install the GameIndex describes. The GameIndex
    /// must outlive the WorldIndex.
    static WorldIndex open(const GameIndex& gi, const WorldIndexOptions& opt = {});

    const WorldIndexInfo& info() const;
    const std::vector<WorldMap>& maps() const;                 // sorted by name
    const std::vector<WorldMloInstance>& mlo_instances() const;
    const std::vector<WorldBounds>& bounds() const;            // sorted by name
    /// Index of a ymap by name hash, -1 if the game has none.
    int find_map(uint32_t name) const;

    /// Maps whose entity extents reach the box.
    std::vector<uint32_t> maps_in(const Vec3& mn, const Vec3& mx, bool groups = false) const;
    /// Entities whose world box reaches the box (loads the maps).
    std::vector<WorldEntity> entities_in(const Vec3& mn, const Vec3& mx, const WorldQuery& q = {}) const;
    /// Entities whose box contains the point, smallest box first (what stands at a world coordinate).
    std::vector<WorldEntity> entities_at(const Vec3& p, const WorldQuery& q = {}) const;
    /// Map collision files whose box reaches the box; interior collision (boxes in interior space) only when asked.
    std::vector<uint32_t> bounds_in(const Vec3& mn, const Vec3& mx, bool interiors = false) const;
    /// The parsed ymap (kept in a small cache; throws rk::Error when the file cannot be read).
    std::shared_ptr<const MapData> map_data(uint32_t map) const;

    struct Impl;
private:
    std::shared_ptr<Impl> d_;
};

} // namespace rk
