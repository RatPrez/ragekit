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
    InteriorRoom = 46, DoorAudioSettings = 47, DoorTuningParams = 48, DoorList = 49, InteractiveMusicMood = 62, StopTrackAction = 64,
    BarConstraint = 70, AlarmSettings = 101, DoorAudioSettingsLink = 117, EntityEmitter = 7,
};
}
const char* dat151_type_name(uint8_t id);

/// Dat54 sound type ids (`Dat54SoundType`).
namespace dat54 {
enum : uint8_t {
    LoopingSound = 1, EnvelopeSound = 2, TwinLoopSound = 3, SpeechSound = 4, OnStopSound = 5, WrapperSound = 6, SequentialSound = 7,
    StreamingSound = 8, RetriggeredOverlappedSound = 9, CrossfadeSound = 10, CollapsingStereoSound = 11, SimpleSound = 12,
    MultitrackSound = 13, RandomizedSound = 14, EnvironmentSound = 15, DynamicEntitySound = 16, SequentialOverlapSound = 17,
    ModularSynthSound = 18, GranularSound = 19, DirectionalSound = 20, KineticSound = 21, SwitchSound = 22, VariableCurveSound = 23,
    VariablePrintValueSound = 24, VariableBlockSound = 25, IfSound = 26, MathOperationSound = 27, ParameterTransformSound = 28,
    FluctuatorSound = 29, AutomationSound = 30, ExternalStreamSound = 31, SoundSet = 32, AutomationNoteMapSound = 33,
    SoundSetList = 34, SoundHashList = 35,
};
}
const char* dat54_type_name(uint8_t id);

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

/// A `.nametable` (`<file>.dat151.nametable` next to the DLC audio data): the names of a data file's items, each NUL
/// terminated, in a row. read gives them in order; write puts them back the same way.
std::vector<std::string> read_nametable(std::span<const uint8_t> file);
Bytes write_nametable(const std::vector<std::string>& names);

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
/// "dasl_<model hash>": the name of the link the game looks a door model's audio up by (any case: the name is hashed lower-cased).
std::string door_audio_link_name(uint32_t model_hash);

struct Dat151DoorAudioSettings {                // type 47: what a door sounds like
    uint32_t name_offset = 0;
    uint32_t sounds = 0;                        // a dat54 SoundSet (script names opening, opened, closing, closed, push, limit, collision ...)
    uint32_t tuning = 0;                        // a DoorTuningParams item
    float    max_occlusion = 0.7f;
    static Dat151DoorAudioSettings read(const RelItem& it);
    Bytes bytes() const;
    std::vector<uint32_t> refs() const;         // the tuning (the sound set lives in another file and is not listed)
};
struct Dat151DoorTuningParams {                 // type 48: when the audio counts a door as open / closed / moving
    uint32_t name_offset = 0;
    uint32_t flags = 0xAAAAAAA8;
    float    open_threshold = 0.98f, heading_threshold = 0, closed_threshold = 0.02f, speed_threshold = 0.1f, speed_scale = 1;
    float    heading_delta_threshold = 0.001f, angular_velocity_threshold = 0.15f;
    int32_t  door_type = 4;                     // the game's: 4 hinged, 2 garage (up and over), 1 vertical (roller shutter)
    static Dat151DoorTuningParams read(const RelItem& it);
    Bytes bytes() const;
    std::vector<uint32_t> refs() const { return {}; }
};
struct Dat151EntityEmitter {                    // type 7: the sound of an archetype's CExtensionDefAudioEmitter (its effectHash names this item)
    uint32_t name_offset = 0;
    uint32_t flags = 0xAAAAAAAA;
    uint32_t sound = 0;                         // a dat54 sound
    float    max_distance = 30;
    float    business_hours_prob = 1, evening_prob = 1, night_prob = 1;   // the chance it plays in each part of the day
    float    cone_inner_angle = 0, cone_outer_angle = 0, cone_max_atten = 0;
    uint32_t stop_after_loud_sound = 0;
    uint8_t  max_path_depth = 3;
    uint8_t  pad0 = 0; uint16_t pad1 = 0;
    float    broken_health = -1;
    static Dat151EntityEmitter read(const RelItem& it);
    Bytes bytes() const;
    std::vector<uint32_t> refs() const { return {}; }
};

// ---------------------------------------------------------------------------------------------------------------
// Typed dat54 sounds. A dat54 item is its type byte, a header (Dat54Header) and the type's own fields.

/// The fields a sound's header can carry, by their bit in its flags word.
namespace dat54_field {
enum : uint8_t {
    Flags2 = 0, MaxHeaderSize = 1, Volume = 2, VolumeVariance = 3, Pitch = 4, PitchVariance = 5, Pan = 6, PanVariance = 7,
    PreDelay = 8, PreDelayVariance = 9, StartOffset = 10, StartOffsetVariance = 11, AttackTime = 12, ReleaseTime = 13,
    DopplerFactor = 14, Category = 15, LPFCutoff = 16, LPFCutoffVariance = 17, HPFCutoff = 18, HPFCutoffVariance = 19,
    VolumeCurve = 20, VolumeCurveScale = 21, VolumeCurvePlateau = 22, SpeakerMask = 23, EffectRoute = 24, PreDelayVariable = 25,
    StartOffsetVariable = 26, SmallReverbSend = 27, MediumReverbSend = 28, LargeReverbSend = 29, Unk30 = 30, Unk31 = 31,
};
}
/// A dat54 sound header: a flags word, then the fields whose bits are set, in bit order (a flags byte of 0xAA = none of
/// its eight fields). Volume is in hundredths of a dB.
struct Dat54Header {
    uint32_t flags = 0xAAAAAAAA;
    uint32_t values[32] = {};                   // by field bit, as stored (a 16-bit field in the low 16 bits: -300 is 0xFED4)
    bool has(uint8_t field) const;
    uint32_t get(uint8_t field) const { return has(field) ? values[field] : 0; }
    /// Sets a field and its bit (a byte of 0xAA starts empty).
    void set(uint8_t field, uint32_t value);
    size_t size() const;                        // bytes, the flags word included
    /// Reads from `b` at `pos` and moves `pos` past the header.
    static Dat54Header read(std::span<const uint8_t> b, size_t& pos);
    void write(Bytes& b) const;
};
struct Dat54SimpleSound {                       // type 12: one wave of a wave pack
    Dat54Header header;
    uint32_t container = 0;                     // joaat_lower("<wave pack folder>/<awc name>"), e.g. "resident/doors"
    uint32_t wave = 0;                          // joaat_lower of the stream's name (the awc keeps its low 29 bits)
    uint8_t  wave_slot = 0;
    static Dat54SimpleSound read(const RelItem& it);
    Bytes bytes() const;
    std::vector<uint32_t> refs() const { return {}; }
    std::vector<uint32_t> pack_refs() const;    // the container
};
struct Dat54SoundSet {                          // type 32: names to sounds (a door's opening, closed ...)
    Dat54Header header;
    std::vector<std::pair<uint32_t, uint32_t>> sounds;   // (joaat_lower of the script name, the sound), in ascending name hash
    static Dat54SoundSet read(const RelItem& it);
    Bytes bytes() const;
    std::vector<uint32_t> refs() const;
};
struct Dat54LoopingSound {                      // type 1: plays its child `loop_count` times (-1 = until stopped)
    Dat54Header header;
    int16_t  loop_count = -1, loop_count_variance = 0, loop_point = 0;
    uint32_t child = 0;
    uint32_t loop_count_variable = 0;
    static Dat54LoopingSound read(const RelItem& it);
    Bytes bytes() const;
    std::vector<uint32_t> refs() const;
};

// Typed items append with these (they pass the right hash refs).
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat151InteriorSettings& v);
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat151InteriorRoom& v);
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat151StaticEmitterList& v);
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat151DoorAudioSettingsLink& v);
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat151DoorAudioSettings& v);
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat151DoorTuningParams& v);
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat151EntityEmitter& v);
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat54SimpleSound& v);
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat54SoundSet& v);
uint32_t rel_add(Rel& r, uint32_t name_hash, const Dat54LoopingSound& v);

} // namespace rk
