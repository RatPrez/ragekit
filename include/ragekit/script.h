// ragekit - compiled scripts (.ysc, versions 10 and 12): the game's mission and world scripts, their bytecode in 16 KB pages,
// statics, globals, the natives they call and their strings.
//
// Every structure is kept as read (graph.h) and written back the same; the views read the header and gather the pages.
// There is no decompiler: the bytecode comes out as bytes.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "graph.h"
#include "types.h"

namespace rk {

enum class ScriptKind : uint16_t { Header = 1, PageArray, CodePage, StringPage, Statics, Globals, Natives, String };

struct Script {
    RscBlockPtr root;   // the header (0x80 bytes)
    int version = 12;   // 12, or 10 (older builds: Enhanced's base scripts, a few Legacy DLC ones; the same layout)

    std::string name() const { return root->string_at(0x60); }
    uint32_t name_hash() const { return root->get<uint32_t>(0x58); }
    uint32_t globals_signature() const { return root->get<uint32_t>(0x18); }
    uint32_t code_size() const { return root->get<uint32_t>(0x1C); }
    uint32_t parameter_count() const { return root->get<uint32_t>(0x20); }
    uint32_t static_count() const { return root->get<uint32_t>(0x24); }
    /// The globals block this script owns: its index in the top bits and the count in the low 18.
    uint32_t global_count() const { return root->get<uint32_t>(0x28) & 0x3FFFF; }
    uint32_t global_block() const { return root->get<uint32_t>(0x28) >> 18; }
    uint32_t native_count() const { return root->get<uint32_t>(0x2C); }
    uint32_t string_size() const { return root->get<uint32_t>(0x70); }

    /// The bytecode, its pages joined.
    Bytes code() const;
    /// The strings' bytes (NUL separated), their pages joined.
    Bytes strings() const;
    /// The statics' initial values, 8 bytes each.
    std::vector<uint64_t> statics() const;
    /// The native hashes the bytecode calls by index: stored rotated, here rotated back (left by code size + index).
    std::vector<uint64_t> natives() const;
};

/// .ysc of either edition. Throws rk::Error for structures the schema does not know; `census` as read_ycd_checked.
Script read_ysc(std::span<const uint8_t> file, size_t* census = nullptr, std::string* where = nullptr);
Bytes write_ysc(const Script& s);

}  // namespace rk
