// ragekit - map files: .ytyp (archetype definitions, incl. interiors) and .ymap (placements).
//
// Both are "meta" files (meta.h). This header is the plain data model. Names are stored as
// the hashes the files hold (joaat of the lowercased name, see hash.h); room names and the
// ymap block description are real strings.
//
// Anything the model has no fields for (archetype / entity extensions, LOD lights, occluder
// meshes, container LODs, composite entity types, archetypes of an unknown type...) is kept as
// MetaObjects, and every structure keeps its original bytes in `raw` (padding and fields the
// model does not name, e.g. the 4th float after a Vec3), so a read followed by a write
// loses nothing but the file's page layout and block order. `raw` is empty for objects you
// create; set nothing there.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include "types.h"
#include "meta.h"

namespace rk {

/// Placed object (CEntityDef). Interiors list these as the content of a room, a ymap
/// lists them as placements; a ymap entity can also be an MLO instance (CMloInstanceDef).
struct Entity {
    uint32_t archetype_name = 0;
    uint32_t flags = 0;
    uint32_t guid = 0;
    Vec3     position;
    Vec4     rotation{0, 0, 0, 1};        // quaternion x y z w
    float    scale_xy = 1.0f;
    float    scale_z = 1.0f;
    int32_t  parent_index = -1;
    float    lod_dist = 0;
    float    child_lod_dist = 0;
    int32_t  lod_level = 0;               // rage__eLodType: 0 HD, 1 LOD, 2 SLOD1, 3 SLOD2, 4 SLOD3, 5 ORPHANHD, 6 SLOD4
    uint32_t num_children = 0;
    int32_t  priority_level = 0;          // rage__ePriorityLevel: 0 required .. 3 optional low
    int32_t  ambient_occlusion_multiplier = 255;
    int32_t  artificial_ambient_occlusion = 255;
    uint32_t tint_value = 0;
    std::vector<MetaObject> extensions;   // CExtensionDefDoor, ...LightEffect, ...SpawnPointOverride, ...

    // Only for an MLO instance placed in a ymap (CMloInstanceDef):
    bool     is_mlo_instance = false;
    uint32_t group_id = 0;
    uint32_t floor_id = 0;
    std::vector<uint32_t> default_entity_sets;
    uint32_t num_exit_portals = 0;
    uint32_t mlo_inst_flags = 0;

    Bytes    raw;                         // the structure's bytes as read: padding and unknown fields are written back from here
};

struct MloRoom {
    std::string name;                     // up to 63 characters
    Vec3     bb_min, bb_max;
    float    blend = 1.0f;
    uint32_t timecycle_name = 0;
    uint32_t secondary_timecycle_name = 0;
    uint32_t flags = 0;
    uint32_t portal_count = 0;
    int32_t  floor_id = 0;
    int32_t  exterior_visibility_depth = -1;
    std::vector<uint32_t> attached_objects; // indices into the interior's entity list
    Bytes    raw;                         // the structure's bytes as read: padding and unknown fields are written back from here
};

struct MloPortal {
    uint32_t room_from = 0;               // room indices (0 = the interior's limbo room)
    uint32_t room_to = 0;
    uint32_t flags = 0;
    uint32_t mirror_priority = 0;
    uint32_t opacity = 0;
    uint32_t audio_occlusion = 0;
    std::vector<Vec3> corners;            // four corners of the opening
    std::vector<float> corner_padding;    // the 4th float stored with each corner as read (0 or NaN); optional, zero when absent
    std::vector<uint32_t> attached_objects;
    Bytes    raw;                         // the structure's bytes as read: padding and unknown fields are written back from here
};

struct MloEntitySet {
    uint32_t name = 0;
    std::vector<uint32_t> locations;      // room indices the set applies to
    std::vector<Entity> entities;
    Bytes    raw;                         // the structure's bytes as read: padding and unknown fields are written back from here
};

struct MloTimeCycleModifier {
    uint32_t name = 0;
    Vec4     sphere;
    float    percentage = 0;
    float    range = 0;
    uint32_t start_hour = 0;
    uint32_t end_hour = 0;
    Bytes    raw;                         // the structure's bytes as read: padding and unknown fields are written back from here
};

struct Archetype {
    enum class Kind : uint8_t {
        Base,   // CBaseArchetypeDef
        Time,   // CTimeArchetypeDef
        Mlo,    // CMloArchetypeDef
        Other,  // any other type: `other` holds it untouched
    };
    Kind     kind = Kind::Base;

    // CBaseArchetypeDef
    float    lod_dist = 100.0f;
    uint32_t flags = 0;
    uint32_t special_attribute = 0;
    Vec3     bb_min, bb_max, bs_centre;
    float    bs_radius = 0;
    float    hd_texture_dist = 100.0f;
    uint32_t name = 0;
    uint32_t texture_dictionary = 0;
    uint32_t clip_dictionary = 0;
    uint32_t drawable_dictionary = 0;
    uint32_t physics_dictionary = 0;
    int32_t  asset_type = 2;              // rage__fwArchetypeDef__eAssetType: 1 fragment, 2 drawable, 3 dictionary, 4 assetless
    uint32_t asset_name = 0;
    std::vector<MetaObject> extensions;

    // CTimeArchetypeDef
    uint32_t time_flags = 0;

    // CMloArchetypeDef
    uint32_t mlo_flags = 0;
    std::vector<Entity>       entities;
    std::vector<MloRoom>      rooms;
    std::vector<MloPortal>    portals;
    std::vector<MloEntitySet> entity_sets;
    std::vector<MloTimeCycleModifier> time_cycle_modifiers;

    MetaObject other;                     // Kind::Other only
    Bytes    raw;                         // the structure's bytes as read: padding and unknown fields are written back from here

    bool is_mlo() const { return kind == Kind::Mlo; }
};

struct MapTypes {
    uint32_t name = 0;
    std::vector<Archetype> archetypes;
    std::vector<MetaObject> extensions;             // file-level extensions
    std::vector<uint32_t> dependencies;
    std::vector<MetaObject> composite_entity_types;
    Bytes    raw;                         // the structure's bytes as read: padding and unknown fields are written back from here

    // The structure / enum descriptions of the file this was read from, reused on write (their
    // keys differ between the tools that made game and mod files). Empty for a new model:
    // the built-in descriptions are used.
    std::vector<MetaStruct> structs;
    std::vector<MetaEnum>   enums;

    Archetype* find(uint32_t name_hash);
    const Archetype* find(uint32_t name_hash) const;
};

struct BoxOccluder {                       // 16 bytes, quantised box
    int16_t center_x = 0, center_y = 0, center_z = 0, cos_z = 0, length = 0, width = 0, height = 0, sin_z = 0;
    bool operator==(const BoxOccluder&) const = default;
};

struct TimeCycleModifier {                 // CTimeCycleModifier (ymap)
    uint32_t name = 0;
    Vec3     min_extents, max_extents;
    float    percentage = 0;
    float    range = 0;
    uint32_t start_hour = 0;
    uint32_t end_hour = 0;
    Bytes    raw;                         // the structure's bytes as read: padding and unknown fields are written back from here
};

struct CarGenerator {                      // CCarGen
    Vec3     position;
    float    orient_x = 0, orient_y = 0;
    float    perpendicular_length = 0;
    uint32_t car_model = 0;
    uint32_t flags = 0;
    int32_t  body_color_remap[4]{-1, -1, -1, -1};
    uint32_t pop_group = 0;
    int8_t   livery = -1;
    Bytes    raw;                         // the structure's bytes as read: padding and unknown fields are written back from here
};

struct BlockDesc {                         // CBlockDesc: who exported the map
    uint32_t version = 0;
    uint32_t flags = 0;
    std::string name, exported_by, owner, time;
};

struct MapData {
    uint32_t name = 0;
    uint32_t parent = 0;
    uint32_t flags = 0;
    uint32_t content_flags = 0;
    Vec3     streaming_extents_min, streaming_extents_max;
    Vec3     entities_extents_min, entities_extents_max;
    std::vector<Entity> entities;
    std::vector<BoxOccluder> box_occluders;
    std::vector<uint32_t> physics_dictionaries;
    std::vector<TimeCycleModifier> time_cycle_modifiers;
    std::vector<CarGenerator> car_generators;
    BlockDesc block;

    // Kept as they are (see the file comment): occluder meshes, container LODs, grass / prop
    // instance data, LOD lights. Empty objects (type 0) mean "none".
    std::vector<MetaObject> occlude_models;
    std::vector<MetaObject> container_lods;
    MetaObject instanced_data;             // rage__fwInstancedMapData (inline in the root structure)
    MetaObject lod_lights;                 // CLODLight
    MetaObject distant_lod_lights;         // CDistantLODLight
    Bytes    raw;                         // the structure's bytes as read: padding and unknown fields are written back from here

    std::vector<MetaStruct> structs;       // see MapTypes
    std::vector<MetaEnum>   enums;
};

// --- conversion to and from the generic container ------------------------------------------------

MapTypes map_types_from_meta(const Meta& m);
MapData  map_data_from_meta(const Meta& m);
Meta     to_meta(const MapTypes& t);
Meta     to_meta(const MapData& d);

/// Typed read / write. Throw rk::Error on malformed files; a structure the file does not
/// describe and the built-in tables do not know cannot be kept inside an extension and is
/// reported (use the generic read_meta / write_meta, which keeps any file).
MapTypes read_ytyp(std::span<const uint8_t> file);
Bytes    write_ytyp(const MapTypes& t, bool compress = true);
MapData  read_ymap(std::span<const uint8_t> file);
Bytes    write_ymap(const MapData& d, bool compress = true);

// --- typed views of what MapData keeps as MetaObjects ------------------------------------------
// The view reads the objects; set_* writes a view back into them, keeping every byte the view has no field for (padding,
// the W lanes of vectors), so set_x(d, x(d)) changes nothing.

/// One instance of a grass batch (rage__fwGrassInstanceListDef__InstanceData, 16 bytes).
struct GrassInstance {
    uint16_t position[3] = {};   // across the batch's box: 0 at its min, 65535 at its max
    uint8_t normal_x = 0, normal_y = 0;
    uint8_t color[3] = {};
    uint8_t scale = 0, ao = 0;
    uint8_t pad[3] = {};
};
static_assert(sizeof(GrassInstance) == 16);

/// A batch of instanced grass (rage__fwGrassInstanceListDef) of a ymap's instanced data.
struct GrassBatch {
    Vec3 box_min, box_max;       // BatchAABB
    Vec3 scale_range;
    uint32_t archetype = 0;
    uint32_t lod_dist = 0;
    float lod_fade_start_dist = 0, lod_inst_fade_range = 0, orient_to_terrain = 0;
    std::vector<GrassInstance> instances;

    Vec3 position(const GrassInstance& g) const;
};
std::vector<GrassBatch> grass_batches(const MapData& d);
void set_grass_batches(MapData& d, const std::vector<GrassBatch>& batches);

/// The LOD lights of a ymap: CLODLight (per light: direction, falloff, exponent, flags, hash, cone and corona bytes) with
/// the position and colour its CDistantLODLight holds for the same index.
struct LodLight {
    Vec3 position;
    uint32_t rgbi = 0;           // colour and intensity bytes
    Vec3 direction;
    float falloff = 0, falloff_exponent = 0;
    uint32_t time_and_state_flags = 0, hash = 0;
    uint8_t cone_inner = 0, cone_outer_or_cap_ext = 0, corona_intensity = 0;
};
struct LodLights {
    std::vector<LodLight> lights;
    uint16_t num_street_lights = 0, category = 0;   // CDistantLODLight
    // Which of the two the ymap holds: the game's `lodlights` ymaps have the CLODLight arrays, its `distlodlights` ones
    // only the distant positions and colours.
    bool has_lod = true, has_distant = true;
};
LodLights lod_lights(const MapData& d);
/// Throws rk::Error when the ymap has no LOD light objects to write into.
void set_lod_lights(MapData& d, const LodLights& l);

/// A field of a detached structure by its name as the game writes it ("offsetPosition": the names hash case and all),
/// through the structure's description (the built-in tables):
/// the bytes and type of the field, nullptr / 0 when the type or the field is not known.
struct MetaField { const uint8_t* data = nullptr; uint8_t type = 0; size_t size = 0; };
MetaField meta_field(const MetaObject& o, std::string_view name);

} // namespace rk
