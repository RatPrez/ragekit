// ragekit - audio data files, the `.rel` container (dat4 / dat10 / dat15 / dat16 / dat22 / dat54 / dat149 /
// dat150 / dat151: audioconfig, speech, synths, mixer, curves, categories, sounds, game data).
//
// A rel file is a plain little endian file:
//
//   u32 type (4, 10, 15, 16, 22, 54, 149, 150, 151)
//   u32 data length, then the data block: a u32 value ("version") followed by the items
//   u32 name table length, u32 name count, count x u32 string offsets, the NUL terminated strings
//   u32 index count, the index: per item (name hash, offset, length) in the data block
//                    (audioconfig.dat4.rel: u32 flags 2524, then per item u8 name length, name, offset, length)
//   u32 hash table count, count x u32 file positions of u32 fields that hold hashes of other items
//   u32 pack table count, count x u32 file positions of u32 fields that hold wave container hashes
//
// `Rel` keeps all of it as stored (item bytes included), so read_rel -> write_rel is byte for byte for every
// game file. Items are opaque byte ranges (RelItem); the typed structs below cover the item types an
// interior needs. The hash/pack tables hold FILE positions, i.e. data block position + 8.
//
// Names: the game hashes item names lower-cased (joaat_lower). The name table of most files is empty; when an
// item has one, bytes 1..3 of the item (types other than dat54) are the byte offset of its name in the strings.
//
// Data file key: the game keys some data files by the file name up to the first underscore and refuses a
// second file with the same key ("failed loading ... in data file mounter"): see rel_data_file_key.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "types.h"
#include "error.h"

namespace rk {

namespace reltype {
enum : uint32_t { Dat4 = 4, Dat10 = 10, Dat15 = 15, Dat16 = 16, Dat22 = 22, Dat54 = 54, Dat149 = 149, Dat150 = 150, Dat151 = 151 };
}
/// "dat151" ... for a rel type, "?" if unknown.
const char* rel_type_name(uint32_t type);
/// The key the game files a rel under: the file name lower-cased and cut at the first underscore ("dlcluxe_game.dat151.rel" -> "dlcluxe").
std::string rel_data_file_key(const std::string& file_name);

/// Dat151 item type ids (`Dat151RelType`) the library knows by name.
namespace dat151 {
enum : uint8_t {
    TrailerAudioSettings = 2, CarAudioSettings = 3, StaticEmitter = 6, TriggeredSpeechContext = 11, SpeechParams = 14,
    WeaponSettings = 17, PedRaceToPedVoiceGroup = 29, StaticEmitterList = 32, ScannerSpecificLocation = 35,
    AmbientZone = 37, AmbientRule = 38, AmbientBankMap = 41, InteriorSettings = 44, InteriorWeaponMetrics = 45,
    InteriorRoom = 46, InteractiveMusicMood = 62, StopTrackAction = 64, BarConstraint = 70, AlarmSettings = 101,
    DoorAudioSettingsLink = 117,
};
}
const char* dat151_type_name(uint8_t id);

/// One index row: where an item lives in the data block.
struct RelIndexEntry {
    uint32_t hash = 0;          // name hash (joaat_lower of `name` for string indexes)
    std::string name;           // string index (audioconfig.dat4.rel) only
    uint32_t offset = 0;        // from the start of the data block (the first u32 sits at 0, so items start at >= 4)
    uint32_t length = 0;
};

/// A view of one item. `bytes` points into Rel::data (valid until the Rel changes).
struct RelItem {
    uint32_t hash = 0;
    std::string name;           // string index only
    uint32_t offset = 0, length = 0;
    uint8_t  type_id = 0;       // first byte of the item
    bool     has_name_offset = false;
    uint32_t name_offset = 0;   // offset into the name table strings (bytes 1..3), when has_name_offset
    std::span<const uint8_t> bytes;
    /// Little endian u32 at byte `pos` of the item (0 if out of range).
    uint32_t u32(size_t pos) const;
};

struct Rel {
    uint32_t type = reltype::Dat151;
    Bytes    data;                           // the data block: u32 "version" then the items
    uint32_t name_table_length = 4;          // as stored (4 + 4 * count + sum(len + 1) when it has names)
    std::vector<uint32_t> name_offsets;      // offsets of the strings inside the string area
    std::vector<std::string> names;
    bool     string_index = false;           // audioconfig.dat4.rel
    uint32_t string_index_flags = 2524;
    std::vector<RelIndexEntry> index;        // in file order
    std::vector<uint32_t> hash_table;        // file positions of hash references
    std::vector<uint32_t> pack_table;        // file positions of wave container references
    Bytes    trailing;                       // bytes after the pack table (none in game files)

    Rel() { data.assign(4, 0); }
    /// An empty rel of that type; `version` is the u32 at the start of the data block.
    static Rel make(uint32_t type, uint32_t version = 0);

    uint32_t version() const;
    void set_version(uint32_t v);

    std::vector<RelItem> items() const;      // in index order
    const RelIndexEntry* find_entry(uint32_t hash) const;
    bool find(uint32_t hash, RelItem& out) const;
    RelItem item(const RelIndexEntry& e) const;
    /// The hash a hash/pack table position refers to (the u32 stored there), nullopt-like: false when out of range.
    bool hash_at(uint32_t file_position, uint32_t& hash) const;
    /// Name of an item through the name table (empty when it has none).
    std::string table_name(const RelItem& it) const;

    /// Alignment the CodeWalker writer pads before an item of this file type / item type (1 = none).
    static uint32_t item_alignment(uint32_t rel_type, uint8_t type_id, bool string_index);

    /// Appends an item and returns its offset. `bytes` are the whole item (type byte first). `hash_refs` and
    /// `pack_refs` are byte positions INSIDE the item of u32 fields that hold hashes of other items / wave
    /// containers (the tables get data offset + 8 + position). The index is kept in the game's order
    /// (hashes rotated right by 8) and the data block is padded like the CodeWalker writer does.
    uint32_t add(uint32_t name_hash, std::span<const uint8_t> bytes, const std::vector<uint32_t>& hash_refs = {},
                 const std::vector<uint32_t>& pack_refs = {});
    /// Index order the game uses (and CodeWalker writes): rotated hash for dat10/15/16/22/54/149/150/151, plain hash for dat4 speech.
    void sort_index();
    /// Sets the name table (offsets and length are derived). Item name offsets (bytes 1..3 of an item) are the caller's business.
    void set_names(const std::vector<std::string>& n);
    /// Checks the internal references: index inside the data block, table positions inside the data block. Empty = fine.
    std::string validate() const;
};

/// Parses a rel file. Throws rk::Error when the structure does not fit the file.
Rel read_rel(std::span<const uint8_t> file);
/// Serialises exactly what the Rel holds (byte-identical to the input of read_rel for an untouched Rel).
Bytes write_rel(const Rel& r);
/// Text listing: header, counts, and per item hash/offset/length/type (`max_items` 0 = all).
std::string rel_describe(const Rel& r, size_t max_items = 0);

// ---------------------------------------------------------------------------------------------------------------
// Typed dat151 items (the types an interior needs). Every struct has `read(const RelItem&)`, `bytes()` (the whole
// item) and `refs()` (positions inside the item for Rel::add's hash_refs).

struct Dat151InteriorSettings {                 // type 44: the MLO
    uint32_t name_offset = 0;
    uint32_t flags = 0xAAAAA004;
    uint32_t walla_sound_set = 0;               // InteriorWallaSoundSet
    uint32_t reflections = 0;                   // InteriorReflections
    std::vector<uint32_t> rooms;                // hashes of the InteriorRoom items
    static Dat151InteriorSettings read(const RelItem& it);
    Bytes bytes() const;
    std::vector<uint32_t> refs() const;
};
struct Dat151InteriorRoom {                     // type 46: one room (index 1.. of the MLO; limbo has none)
    uint32_t name_offset = 0;
    uint32_t flags = 0xAAAAAAAA;
    uint32_t room_name = 0;                     // hash of the room name as in the ytyp
    uint32_t ambient_zone = 0;
    uint8_t  interior_type = 0;
    uint8_t  pad0 = 0; uint16_t pad1 = 0;
    float    reverb_small = 0, reverb_medium = 0, reverb_large = 0;
    uint32_t room_tone_sound = 0;
    uint8_t  rain_type = 0;
    uint8_t  pad2 = 0; uint16_t pad3 = 0;
    float    exterior_audibility = 0, room_occlusion_damping = 0, non_marked_portal_occlusion = 0;
    float    distance_from_portal_for_occlusion = 0, distance_from_portal_fade_distance = 0;
    uint32_t weapon_metrics = 0;
    uint32_t walla_sound_set = 0;
    static Dat151InteriorRoom read(const RelItem& it);
    Bytes bytes() const;
    std::vector<uint32_t> refs() const;
};
struct Dat151StaticEmitterList {                // type 32
    uint32_t name_offset = 0;
    std::vector<uint32_t> emitters;
    static Dat151StaticEmitterList read(const RelItem& it);
    Bytes bytes() const;
    std::vector<uint32_t> refs() const;
};
struct Dat151DoorAudioSettingsLink {            // type 117, named dasl_<model hash as 8 hex digits>
    uint32_t name_offset = 0;
    uint32_t door = 0;                          // hash of the DoorAudioSettings / DoorList item
    static Dat151DoorAudioSettingsLink read(const RelItem& it);
    Bytes bytes() const;
    std::vector<uint32_t> refs() const;
};

// Typed items append with these (they pass the right hash refs).
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat151InteriorSettings& v);
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat151InteriorRoom& v);
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat151StaticEmitterList& v);
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat151DoorAudioSettingsLink& v);

} // namespace rk
