// ragekit - "meta" resources: the container behind .ytyp, .ymap and a few more.
//
// A meta file is an RSC7 (version 2) holding a table of structure descriptions, a table
// of enum descriptions and a list of data blocks. Each block is the packed instances of one
// structure type (or raw strings / arrays: the block type is then a small number). Structures
// refer to each other with 32-bit "meta pointers": the block's 1-based id in the low 12 bits
// and the byte offset inside the block in the next 20.
//
// This header is the generic layer: it reads and writes the container without knowing what
// the blocks mean, so any meta file survives a read/write pass. map.h builds typed .ytyp and
// .ymap on top of it.
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "types.h"
#include "error.h"

namespace rk {

namespace metatype {
// Block "type hashes" of the non-structure blocks (CodeWalker's MetaTypeName).
constexpr uint32_t kVector4 = 0x33;
constexpr uint32_t kHash    = 0x4A;
constexpr uint32_t kString  = 0x10;
constexpr uint32_t kPointer = 0x07;
constexpr uint32_t kUShort  = 0x13;
constexpr uint32_t kUInt    = 0x15;
constexpr uint32_t kByte    = 0x11;
constexpr uint32_t kFloat   = 0x21;
constexpr uint32_t kArrayInfo = 0x100; // name of the element-description entry before an Array entry

// Data types of structure entries (MetaStructureEntryDataType).
enum Entry : uint8_t {
    Boolean = 0x01, SignedByte = 0x10, UnsignedByte = 0x11, SignedShort = 0x12, UnsignedShort = 0x13,
    SignedInt = 0x14, UnsignedInt = 0x15, Float = 0x21, Float_XYZ = 0x33, Float_XYZW = 0x34,
    ByteEnum = 0x60, IntEnum = 0x62, IntFlags1 = 0x63, ShortFlags = 0x64, IntFlags2 = 0x65,
    Hash = 0x4A, Array = 0x52, ArrayOfChars = 0x40, ArrayOfBytes = 0x50, DataBlockPointer = 0x59,
    CharPointer = 0x44, StructurePointer = 0x07, Structure = 0x05,
};
} // namespace metatype

/// One field of a structure description (16 bytes in the file).
struct MetaEntry {
    uint32_t name = 0;        // joaat of the field name (0x100 = element description of the next Array)
    int32_t  offset = 0;      // byte offset in the structure
    uint8_t  type = 0;        // metatype::Entry
    uint8_t  unknown9 = 0;
    int16_t  ref_index = 0;   // Array: index of its element-description entry
    uint32_t ref_key = 0;     // Structure/enum: name hash of the structure/enum; ArrayOfX: length
};
/// Structure description (32 bytes + entries).
struct MetaStruct {
    uint32_t name = 0;
    uint32_t key = 0;
    uint32_t unknown8 = 0;    // the alignment's log2 << 8: 1024 (16 bytes), 768 (8), 512 (4), 256 (2), 0 (1); see alignment()
    uint32_t unknownC = 0;    // 0 in every game file
    int32_t  size = 0;        // bytes per instance
    uint16_t unknown1C = 0;   // 0 in every game file
    std::vector<MetaEntry> entries;

    /// The structure's alignment in bytes (1 << (unknown8 >> 8)): what its widest field needs on 97.8 % of the game's
    /// 39,225 descriptions (docs/unknowns.md).
    uint32_t alignment() const { return 1u << ((unknown8 >> 8) & 31); }
};
struct MetaEnumEntry { uint32_t name = 0; int32_t value = 0; };
struct MetaEnum {
    uint32_t name = 0;
    uint32_t key = 0;
    int32_t  unknown14 = 0;
    std::vector<MetaEnumEntry> entries;
};
/// A data block: `type` is the structure name hash (or a metatype:: number for strings / arrays).
struct MetaBlock {
    uint32_t type = 0;
    Bytes    data;
};

/// The 16 byte array header structures use for every array and string field.
struct MetaArray {
    uint64_t pointer = 0;     // meta pointer to the first element (0 when empty)
    uint16_t count = 0;
    uint16_t capacity = 0;
    uint32_t unknown = 0;
};

// Meta pointers: block id (1-based) in bits 0-11, byte offset in bits 12-31.
inline uint32_t meta_block_id(uint64_t p)  { return uint32_t(p & 0xFFF); }
inline uint32_t meta_offset(uint64_t p)    { return uint32_t((p >> 12) & 0xFFFFF); }
inline uint64_t make_meta_pointer(uint32_t block_id, uint32_t offset) {
    return uint64_t(block_id & 0xFFF) | (uint64_t(offset & 0xFFFFF) << 12);
}

struct Meta {
    std::string name;                       // the name the file carries (often empty)
    // Header fields CodeWalker calls Unknown_*; game files always carry the defaults.
    uint32_t signature = 0x50524430;        // "0DRP"
    uint16_t header_version = 0x79;
    uint8_t  unknown17 = 0;
    uint32_t unknown18 = 0;
    uint16_t unknown4e = 0;
    std::array<uint32_t, 8> unknown50{};
    int32_t  root_block = 1;                // 1-based id of the block holding the root structure
    bool     has_encrypted_strings = false; // the strings block is kept verbatim, never decoded
    Bytes    encrypted_strings;

    std::vector<MetaStruct> structs;
    std::vector<MetaEnum>   enums;
    std::vector<MetaBlock>  blocks;

    // --- lookups ---------------------------------------------------------------------------
    const MetaStruct* find_struct(uint32_t name_hash) const;
    const MetaEnum*   find_enum(uint32_t name_hash) const;
    /// First block of this type (nullptr if none).
    const MetaBlock*  find_block(uint32_t type) const;
    const MetaBlock*  root() const;
    /// Block by 1-based id; throws Error when out of range.
    const MetaBlock&  block(uint32_t id) const;

    // --- pointers --------------------------------------------------------------------------
    /// The bytes a meta pointer points at, checking `need` bytes fit in the block. Throws.
    const uint8_t* at(uint64_t pointer, size_t need) const;
    /// Type of the block a pointer points into.
    uint32_t type_at(uint64_t pointer) const { return block(meta_block_id(pointer)).type; }
    /// The elements of an array of fixed-size items (stride bytes each). Empty for a null array.
    std::span<const uint8_t> array_bytes(const MetaArray& a, size_t stride) const;
    /// NUL-terminated string at a pointer / a CharPointer-style array (uses the pointer only).
    std::string string_at(uint64_t pointer) const;
    /// Reads a 16 byte MetaArray at a location inside a structure.
    static MetaArray read_array(const uint8_t* p);
};

/// Parses a complete .ytyp/.ymap/... file. Throws rk::Error (including for PSO-style meta files).
Meta read_meta(std::span<const uint8_t> file);
/// Serialises. Keeps block order and contents exactly as in `m`.
Bytes write_meta(const Meta& m, bool compress = true);

/// The structure / enum descriptions CodeWalker knows (the ones game map files use). nullptr if unknown.
const MetaStruct* known_struct(uint32_t name_hash);
const MetaEnum*   known_enum(uint32_t name_hash);

struct MetaObject;

/// One thing a detached structure points at (see MetaObject).
struct MetaLink {
    enum class Kind : uint8_t {
        String,       // CharPointer field: `raw` holds the characters (no terminator)
        Structs,      // Array of Structure: `items` are contiguous elements
        Pointers,     // Array of StructurePointer: each item is a separate structure
        Primitives,   // Array of numbers / hashes / vectors: `raw` holds count * stride bytes
        DataBlock,    // DataBlockPointer: `raw` is a block of its own
        Pointer,      // a single StructurePointer field: items[0]
    };
    Kind     kind = Kind::String;
    uint32_t offset = 0;      // of the field in MetaObject::data
    uint32_t block_type = 0;  // Structs: element type; Primitives / DataBlock: type of the block it lived in
    uint32_t count = 0;       // Primitives: element count
    Bytes    raw;
    std::vector<MetaObject> items;
};

/// A structure instance lifted out of its file: its bytes with the pointer fields cleared
/// plus everything those pointers led to, so it can be moved to another Meta.
/// This is how map.h keeps extension structures (and other types it has no typed view of)
/// intact. The description of the type must be known (the file's own, or the built-in tables).
struct MetaObject {
    uint32_t type = 0;
    Bytes    data;            // exactly the structure's size
    std::vector<MetaLink> links;
};

/// Detaches the structure of type `type` whose bytes start at `instance`. Throws Error when a
/// type involved has no description in `m` or the built-in tables.
MetaObject detach_object(const Meta& m, uint32_t type, const uint8_t* instance, size_t available);
/// Detaches whatever a meta pointer points at (the type is the block's type).
MetaObject detach_object(const Meta& m, uint64_t pointer);

/// A location written by MetaBuilder.
struct MetaPtr {
    uint32_t block_id = 0;
    uint32_t offset = 0;
    uint32_t length = 0;      // items in an array / characters in a string
    uint64_t pointer() const { return make_meta_pointer(block_id, offset); }
    MetaArray array() const { MetaArray a; a.pointer = block_id ? pointer() : 0; a.count = a.capacity = (uint16_t)length; return a; }
};

/// Packs structures the way CodeWalker's MetaBuilder does: one block per structure type
/// (a new block of the same type once one passes 16 KB), every item padded to 16 bytes.
class MetaBuilder {
public:
    /// One structure instance (padded to a multiple of 16 bytes) in the block of `type`.
    MetaPtr add_item(uint32_t type, std::span<const uint8_t> data);
    /// `count` contiguous items in one go (an Array of Structure); data is padded to 16.
    MetaPtr add_item_array(uint32_t type, std::span<const uint8_t> data, uint32_t count);
    /// NUL-terminated string in the string block; length excludes the terminator.
    MetaPtr add_string(const std::string& s);
    /// A block of its own (e.g. occluder vertices).
    MetaPtr add_data_block(std::span<const uint8_t> data, uint32_t type);
    /// Array of pointers to separately added items (an Array of StructurePointer).
    MetaPtr add_pointer_array(std::span<const uint64_t> pointers);
    MetaPtr add_uint_array(std::span<const uint32_t> v, uint32_t type = metatype::kUInt);
    MetaPtr add_ushort_array(std::span<const uint16_t> v);
    MetaPtr add_byte_array(std::span<const uint8_t> v);
    MetaPtr add_float_array(std::span<const float> v);
    /// Vector3s padded to 16 bytes each, as portal corners are stored; `w` gives the 4th float of
    /// each (zero where missing).
    MetaPtr add_padded_vec3_array(std::span<const Vec3> v, std::span<const float> w = {});

    /// Writes everything a detached object points at and returns its bytes with the pointers
    /// filled in (to be placed by the caller inside a bigger structure, or via add_object).
    Bytes pack_object(const MetaObject& o);
    /// A detached object as an item of its own type.
    MetaPtr add_object(const MetaObject& o);
    /// Several objects of one type as a contiguous array (Array of Structure).
    MetaPtr add_object_array(uint32_t type, std::span<const MetaObject> v);
    /// Objects as separate items plus the pointer array to them (Array of StructurePointer).
    MetaPtr add_object_pointers(std::span<const MetaObject> v);

    /// Registers the description of a structure / enum for the output (looked up in the
    /// built-in tables). Throws if unknown.
    void use_struct(uint32_t name_hash);
    void use_enum(uint32_t name_hash);
    /// Registers a description taken from somewhere else (e.g. read from the source file).
    void use_struct(const MetaStruct& s);
    void use_enum(const MetaEnum& e);

    /// Makes sure a block of this type exists now, so the root structure gets id 1 even though
    /// it is added after everything it points to.
    void reserve_block(uint32_t type) { ensure(type); }

    /// Builds the Meta; the first block created becomes the root (id 1).
    /// Structure and enum descriptions are added for every block type that has one, plus
    /// everything their entries refer to; `preferred_*` (usually the source file's own tables)
    /// win over the built-in ones.
    Meta finish(const std::string& name = "", const std::vector<MetaStruct>* preferred_structs = nullptr,
                const std::vector<MetaEnum>* preferred_enums = nullptr) const;

private:
    MetaBlock& ensure(uint32_t type);
    std::vector<MetaBlock> blocks_;
    std::vector<MetaStruct> structs_;
    std::vector<MetaEnum> enums_;
};

} // namespace rk
