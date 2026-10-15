// ragekit - audio for an interior (MLO): occlusion and room settings.
//
// Without it every sound in an MLO is heard at full volume through the walls and the rooms have no reverb.
// Two files fix that, both keyed on the interior:
//
//   stream/<proxy hash>.ymt        naOcclusionInteriorMetadata (a PSO): which portals connect which rooms
//                                  (with the door / glass entities in them) and the sound paths between rooms
//   audio/<name>_game.dat151.rel   one InteriorSettings (the MLO) and one InteriorRoom per room: reverb, tone ...
//                                  (data_file 'AUDIO_GAMEDATA' 'audio/<name>_game.dat'; restart the server, not the resource)
//
// The recipe is verified against the game's own file for v_apart_midspaz (the algorithm is ht_mlotool's /
// gtav-audio-occlusion's, MIT):
//   proxy hash  = joaat(archetype) ^ trunc(x*100) ^ trunc(y*100) ^ trunc(z*100)  of the ymap position
//   room key    = proxy ^ joaat(room name); the limbo room ("limbo") uses joaat("outside")
//   path key    = (key(from) - key(to)) mod 2^32 + hops
// The `.ymt` file name is the proxy hash in decimal.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "types.h"
#include "error.h"
#include "rel.h"
#include "map.h"

namespace rk {

// --- the file content -------------------------------------------------------------------------------------------

struct OcclusionPortalEntity {                  // naOcclusionPortalEntityMetadata
    uint32_t link_type = 1;
    float    max_occlusion = 0.7f;              // the game's apartment doors use 0.7
    uint32_t model_hash = 0;                    // joaat of the door / window model
    bool     is_door = true;
    bool     is_glass = false;
};
struct OcclusionPortalInfo {                    // naOcclusionPortalInfoMetadata
    uint32_t interior_proxy_hash = 0;
    int32_t  portal_idx = 0;                    // index inside the room (mirror portals count)
    int32_t  room_idx = 0;
    uint32_t dest_interior_hash = 0;
    int32_t  dest_room_idx = 0;
    std::vector<OcclusionPortalEntity> entities;
};
struct OcclusionPathChild { uint32_t path_node_key = 0; int32_t portal_info_idx = 0; };   // naOcclusionPathNodeChildMetadata
struct OcclusionPathNode {                      // naOcclusionPathNodeMetadata
    uint32_t key = 0;
    std::vector<OcclusionPathChild> children;
};
/// Everything in the `.ymt` (naOcclusionInteriorMetadata).
struct OcclusionData {
    std::vector<OcclusionPortalInfo> portals;   // PortalInfoList
    std::vector<OcclusionPathNode>   nodes;     // PathNodeList (a hash keyed list: the game finds nodes by key)
};

/// Reads a `<proxy>.ymt` (a PSO; game files and CodeWalker-built ones). Throws rk::Error.
OcclusionData read_occlusion_ymt(std::span<const uint8_t> file);
/// Writes it the way CodeWalker's PsoBuilder does (sections PSIN, PMAP, PSCH; blocks in creation order: entities, portals,
/// path children, path nodes, root).
Bytes write_occlusion_ymt(const OcclusionData& d);

// --- the model an editor fills ----------------------------------------------------------------------------------

struct OcclusionEntity {                        // a door or window sitting in a portal
    std::string model;                          // archetype name ("hash_XXXXXXXX" is taken as the hash itself)
    uint32_t model_hash = 0;                    // used when `model` is empty
    uint32_t link_type = 1;
    float    max_occlusion = 0.7f;
    bool     is_door = true;
    bool     is_glass = false;
};
struct OcclusionPortal {
    uint32_t room_from = 0, room_to = 0;
    uint32_t flags = 0;                         // bit 2 (4) = mirror portal: kept in the numbering, no occlusion entry
    std::vector<OcclusionEntity> entities;
    bool mirror() const { return (flags & 4) != 0; }
};
struct AudioOcclusion {
    std::string name;                           // the MLO archetype name
    uint32_t name_hash = 0;                     // joaat of the name; used for the proxy hash when `name` is empty
    Vec3 position;                              // where the MLO is placed (ymap), for the proxy hash
    std::vector<std::string> rooms;             // ytyp order; index 0 is "limbo"
    std::vector<OcclusionPortal> portals;       // ytyp order
    int max_distance = 5;                       // longest path (portal hops) generated; the game's files have 1..3
    uint32_t proxy_hash() const;
};

/// joaat(name) ^ trunc(x*100) ^ trunc(y*100) ^ trunc(z*100) (float arithmetic, truncation toward zero).
uint32_t occlusion_proxy_hash(const std::string& archetype, const Vec3& position);
uint32_t occlusion_proxy_hash(uint32_t archetype_hash, const Vec3& position);
/// The portal list and the paths between all rooms.
OcclusionData build_occlusion(const AudioOcclusion& m);
inline Bytes write_occlusion_ymt(const AudioOcclusion& m) { return write_occlusion_ymt(build_occlusion(m)); }

/// An MLO archetype of a ytyp placed at `position`: rooms by name, portals with the entities attached to them
/// (the archetype names of the interior's entities listed in the portal's attached objects).
AudioOcclusion occlusion_from_mlo(const Archetype& mlo, const Vec3& position);

// --- dat151 ------------------------------------------------------------------------------------------------------

/// Per room values of the InteriorRoom item; the defaults are the game's v_apart_midspaz rooms.
struct RoomAudioSettings {
    uint32_t flags = 0xAAAAAAAA;
    std::string ambient_zone;                   // name or hash_XXXXXXXX; empty = none
    uint8_t  interior_type = 0;
    float    reverb_small = 0.3f, reverb_medium = 0.0f, reverb_large = 0.0f;
    std::string room_tone_sound = "null_sound";
    uint8_t  rain_type = 0;
    float    exterior_audibility = 0, room_occlusion_damping = 0, non_marked_portal_occlusion = 0.8f;
    float    distance_from_portal_for_occlusion = 0, distance_from_portal_fade_distance = 15;
    std::string weapon_metrics = "hash_C3967E5B";
    std::string walla_sound_set = "hash_D4855127";
};
struct InteriorAudioSettings {
    uint32_t flags = 0xAAAAA004;
    std::string walla_sound_set = "hash_D4855127";
    std::string reflections;                    // empty = none
    bool case_sensitive_names = false;          // hash item / room names as typed (what CodeWalker's XML import does) instead of lower-cased (what the game does)
    RoomAudioSettings default_room;             // used for rooms without an entry in `rooms`
    std::vector<std::pair<std::string, RoomAudioSettings>> rooms; // by room name
};
/// The `<name>_game.dat151.rel`: InteriorSettings "<name>" listing "<name>_<room>" and one InteriorRoom per room
/// (RoomName = the room's name; limbo, room 0, has none). Item order: settings, then the rooms.
Rel build_interior_dat151(const std::string& name, const std::vector<std::string>& rooms, const InteriorAudioSettings& s = {});
inline Rel build_interior_dat151(const AudioOcclusion& m, const InteriorAudioSettings& s = {}) { return build_interior_dat151(m.name, m.rooms, s); }

/// A file name for the interior's rel whose data file key (see rel_data_file_key) is unique to the interior: the archetype
/// name without its underscores, then `_game.dat151.rel` ("rat_apt01" -> "ratapt01_game.dat151.rel"). A name like
/// `rat_apt01_game.dat151.rel` has the key "rat" and a second `rat_` rel is refused by the game.
std::string interior_dat151_file_name(const std::string& archetype);

/// "hash_XXXXXXXX" / "0xXXXXXXXX" are taken as the hash, anything else is hashed lower-cased (empty = 0).
uint32_t audio_hash(const std::string& name_or_hash);

} // namespace rk
