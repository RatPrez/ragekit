// ragekit - PSO, the second "meta" container (magic "PSIN"): binary .ymt (ped variation infos,
// carcols, carvariations, handling, pedpersonality ...), .ypdb and a few more.
//
// Unlike Meta (an RSC7 resource) a PSO is a plain file, big endian throughout: a list of
// sections, each `ident(4) length(4) payload`, where length counts the 8 header bytes.
//
//   PSIN  the data: 8 'p' bytes of padding, then the blocks back to back
//   PMAP  the root block id and the list of blocks (name hash, offset, length)
//   PSCH  the schema: structure infos and enum infos with their entries
//   STRF / STRS / STRE / PSIG   string tables and a signature, kept as raw payload
//   CHKS  file size + checksum (+ "yppp"), recomputed by update_checksum()
//
// This header is the generic layer, like meta.h: read_pso/write_pso know the container only,
// so a file survives a read/write pass byte for byte; PsoView walks the data with the file's
// own schema (typed access by name hash) and pso_dump prints it as text.
#pragma once
#include <cstdint>
#include <span>
#include <unordered_map>
#include <string>
#include <vector>
#include "types.h"
#include "error.h"

namespace rk {

namespace psotype {
// PsoDataType (CodeWalker). Entry types in the schema.
enum : uint8_t {
    Bool = 0x00, SByte = 0x01, UByte = 0x02, SShort = 0x03, UShort = 0x04, SInt = 0x05, UInt = 0x06,
    Float = 0x07, Float2 = 0x08, Float3 = 0x09, Float4 = 0x0A, String = 0x0B, Structure = 0x0C,
    Array = 0x0D, Enum = 0x0E, Flags = 0x0F, Map = 0x10, Float3a = 0x14, Float4a = 0x15,
    HFloat = 0x1E, Long = 0x20,
};
const char* name(uint8_t t);
// Name hashes with a meaning of their own.
constexpr uint32_t kArrayInfo = 0x100;    // "element description" entry in front of an Array entry
constexpr uint32_t kPointerBlock = 12;    // block type of an array of structure pointers (PsoPOINTER)
} // namespace psotype

/// One field of a structure info (12 bytes in the file).
struct PsoStructEntry {
    uint32_t name = 0;        // field name hash (0x100 = element description of the next Array entry)
    uint8_t  type = 0;        // psotype::
    uint8_t  subtype = 0;     // CodeWalker "Unk_5h": how the type is stored (see PsoView)
    uint16_t offset = 0;      // byte offset in the structure
    uint32_t ref_key = 0;     // Structure/Enum: name hash; Array: count << 16 | element entry index; String: length << 16; ...
};
struct PsoStructInfo {
    uint8_t  kind = 0;        // 0 = structure
    uint8_t  unknown = 0;     // CodeWalker "Unk"
    int32_t  size = 0;        // bytes per instance
    uint32_t unknown_c = 0;
    std::vector<PsoStructEntry> entries;
};
struct PsoEnumEntry { uint32_t name = 0; int32_t value = 0; };
struct PsoEnumInfo {
    uint8_t kind = 1;         // 1 = enum
    std::vector<PsoEnumEntry> entries;
};
/// A schema element: a structure info or an enum info, in file order.
struct PsoElement {
    uint32_t name = 0;
    bool is_enum = false;
    PsoStructInfo st;
    PsoEnumInfo   en;
};
struct PsoSchema {
    std::vector<PsoElement> elements;
    /// First element of that name (structure / enum). nullptr if none.
    const PsoStructInfo* find_struct(uint32_t name) const;
    const PsoEnumInfo*   find_enum(uint32_t name) const;
};

/// A data block (PMAP entry): `offset` is a position in the PSIN section *including* its 8 header bytes.
struct PsoBlock {
    uint32_t name = 0;        // structure name hash, or a small number (1 = strings / raw, 12 = structure pointers, ...)
    int32_t  offset = 0;
    int32_t  unknown = 0;
    int32_t  length = 0;
};
struct PsoDataMap {
    int32_t  root_id = 1;     // 1-based block id of the root structure
    uint16_t unknown = 0x7070;
    bool     extended = false;// the rare header variant with a zero count followed by the real one
    uint16_t ext[4] = {0, 0, 0, 0};   // count, 3 unknown (extended only)
    std::vector<PsoBlock> blocks;
};
struct PsoChecks {
    uint32_t file_size = 0;
    uint32_t checksum = 0;
    uint32_t tag = 0x79707070; // "yppp"
};

/// A section as stored. When `parsed` the payload is regenerated from Pso::map / schema / checks on writing,
/// otherwise it is written verbatim (unknown sections, string tables, anything whose re-encoding would differ).
struct PsoSection {
    uint32_t ident = 0;
    Bytes    payload;         // bytes after the 8 byte header
    bool     parsed = false;
};

struct Pso {
    std::vector<PsoSection> sections;   // in file order
    Bytes    trailing;                  // bytes after the last section (normally none)
    PsoDataMap map;                     // from the PMAP section
    PsoSchema  schema;                  // from the PSCH section
    PsoChecks  checks;                  // from the CHKS section
    bool has_map = false, has_schema = false, has_checks = false;

    /// First section with that ident (nullptr if none).
    PsoSection*       find_section(uint32_t ident);
    const PsoSection* find_section(uint32_t ident) const;
    /// The PSIN payload (8 'p' padding bytes + blocks). Block offsets count from the start of the section, header included.
    Bytes&       data();
    const Bytes& data() const;
    /// Block by 1-based id (nullptr if out of range / no map).
    const PsoBlock* block(uint32_t id) const;
    /// Position of byte `offset` of block `id` inside data() (block offsets include the 8 header bytes).
    size_t data_pos(const PsoBlock& b, size_t offset = 0) const { return size_t(b.offset) + offset - 8; }
    /// The NUL separated strings of a STRF / STRS section (empty if absent).
    std::vector<std::string> strings(uint32_t ident) const;
    /// Recomputes CHKS file size and checksum for the file as write_pso would produce it.
    void update_checksum();
};

constexpr uint32_t kPsoPSIN = 0x5053494E, kPsoPMAP = 0x504D4150, kPsoPSCH = 0x50534348, kPsoPSIG = 0x50534947,
                   kPsoSTRF = 0x53545246, kPsoSTRS = 0x53545253, kPsoSTRE = 0x53545245, kPsoCHKS = 0x43484B53;

/// True when the bytes start with "PSIN".
bool is_pso(std::span<const uint8_t> file);
/// Parses a PSO file. Throws rk::Error when it is not one or the sections do not fit the file.
Pso read_pso(std::span<const uint8_t> file);
/// Serialises; byte-identical to the input of read_pso for an untouched Pso.
Bytes write_pso(const Pso& p);
/// The checksum the game files carry (Jenkins one-at-a-time with seed 0x3FAC7125 over the whole file with
/// the CHKS size and checksum fields zeroed).
uint32_t pso_checksum(std::span<const uint8_t> file);

/// Name for a hash: the CodeWalker MetaName table if it knows it, else "hash_XXXXXXXX".
std::string pso_name(uint32_t hash);

// ---------------------------------------------------------------------------------------------------------------
// Generic walker

class PsoView;

/// A structure instance inside the data: where it is and which structure info describes it.
struct PsoObject {
    const PsoView* view = nullptr;
    uint32_t type = 0;          // structure name hash
    size_t   pos = 0;           // absolute position in data()
    const PsoStructInfo* info = nullptr;
    explicit operator bool() const { return view && info; }
};

/// One field of an object. `entry` is the schema entry; `pos` is where the value (or array header) starts.
struct PsoField {
    PsoObject owner;
    const PsoStructEntry* entry = nullptr;
    size_t pos = 0;
    uint32_t name() const { return entry ? entry->name : 0; }
};

/// A decoded array: where the elements are, how many, and the element description.
struct PsoArrayRef {
    size_t   pos = 0;                      // position of the first element in data()
    uint32_t count = 0;
    const PsoStructEntry* element = nullptr; // the element description entry (the ARRAYINFO entry)
    bool     pointers = false;             // elements are 8 byte pointers to separate structures
    size_t   stride = 0;                   // bytes between elements (0 = unknown)
    bool     valid = false;
};

/// Walks the data section with the file's schema. The Pso must outlive the view.
class PsoView {
public:
    /// Collects the names the file itself carries (STRF / STRS strings, hashed) to resolve hash values.
    explicit PsoView(const Pso& p);
    /// Name of a hash: the file's own strings first, then the MetaName table, else hash_XXXXXXXX. Hash 0 gives "".
    std::string name(uint32_t hash) const;
    const Pso& pso() const { return pso_; }

    /// The root object (the block named by PMAP root id). Empty object when the file has none.
    PsoObject root() const;
    /// Object at (1-based block id, offset inside the block). Type = the block's name unless `type` is given.
    PsoObject at(uint32_t block_id, uint32_t offset, uint32_t type = 0) const;
    /// Object an 8 byte pointer field points at, given as the 8 bytes read big endian (first word: block id | offset << 12).
    PsoObject follow(uint64_t pointer, uint32_t type = 0) const;

    /// Fields in schema order (ARRAYINFO entries skipped).
    std::vector<PsoField> fields(const PsoObject& o) const;
    const PsoField* find(const PsoObject& o, uint32_t name, PsoField& storage) const;

    // typed reads (big endian decoded). Return false when the field is not of a compatible type.
    bool get_int(const PsoField& f, int64_t& v) const;       // bool/ints/enum/flags/hash-as-uint
    bool get_float(const PsoField& f, double& v) const;      // float, half-float
    bool get_vec(const PsoField& f, float out[4], int& n) const; // Float2/3/4 (n = components)
    bool get_string(const PsoField& f, std::string& s) const; // String fields (char arrays, pointers, hashes)
    /// Nested structure / structure pointer fields.
    PsoObject get_struct(const PsoField& f) const;
    /// The array behind an Array field.
    PsoArrayRef get_array(const PsoField& f) const;
    /// Same for an Array entry of `owner` whose value (or header) starts at `pos` (nested arrays, map internals).
    PsoArrayRef array_at(const PsoObject& owner, const PsoStructEntry& entry, size_t pos) const;
    /// Element i of a Float2/3/4 array (n = components).
    bool array_vec(const PsoArrayRef& a, uint32_t i, float out[4], int& n) const;
    /// Element i of a string array (hash arrays give the name, pointer arrays the text).
    bool array_string(const PsoArrayRef& a, uint32_t i, std::string& s) const;
    /// Element i of a structure / structure pointer array.
    PsoObject array_struct(const PsoArrayRef& a, uint32_t i) const;
    /// Element i of a primitive array as integer / float (false if the element type is not of that kind).
    bool array_int(const PsoArrayRef& a, uint32_t i, int64_t& v) const;
    bool array_float(const PsoArrayRef& a, uint32_t i, double& v) const;
    /// Name of an enum value (empty string when unknown).
    std::string enum_name(uint32_t enum_hash, int64_t value) const;
    /// Flag names set in `value` (space separated), empty when the enum is unknown.
    std::string flag_names(uint32_t enum_hash, uint64_t value) const;

    /// Entries of a Map field. Items are structures (`object`) or strings (`text`, is_string).
    struct MapItem { std::string key; bool is_string = false; std::string text; PsoObject object; };
    std::vector<MapItem> get_map(const PsoField& f) const;
    /// Text of a string-typed field at `pos` described by `entry` (also used for array elements and map keys).
    bool string_at(const PsoStructEntry& entry, size_t pos, std::string& s) const;

private:
    const Pso& pso_;
    std::unordered_map<uint32_t, std::string> names_;
};

/// Generic text dump of the root object (and everything it reaches). `max_items` limits long arrays (0 = all).
/// With `flat`: one "path<TAB>value" line per scalar or primitive array instead (for machine comparison).
std::string pso_dump(const Pso& p, size_t max_items = 0, bool flat = false);

/// One line per section, structure info and enum info (`rk info -v`).
std::string pso_describe(const Pso& p, bool verbose);

} // namespace rk
