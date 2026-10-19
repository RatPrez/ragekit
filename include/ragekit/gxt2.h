// ragekit - .gxt2 text tables: the game's strings (labels, subtitles, help texts) keyed by the joaat of their label, one
// file per language and text block.
//
// Layout (little endian): "2TXG", the entry count, (hash, offset) per entry, "2TXG" again, the file's size, then the
// strings, UTF-8 and NUL terminated, in the order of the entries. The game's files keep their entries sorted by hash.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "types.h"

namespace rk {

struct TextTable {
    struct Entry {
        uint32_t hash = 0;     // joaat of the label ("hash_or_name" takes either)
        std::string text;      // UTF-8, without the NUL
    };
    std::vector<Entry> entries;

    /// The text of a label's hash, nullptr when the table has none.
    const std::string* find(uint32_t hash) const;
    /// Sets (or adds, keeping the entries sorted by hash) a label's text.
    void set(uint32_t hash, std::string text);
};

bool is_gxt2(std::span<const uint8_t> file);
/// Throws rk::Error for a file that is not a text table or whose offsets lie outside it.
TextTable read_gxt2(std::span<const uint8_t> file);
/// The entries in their order, each string once after the table: the game's own layout, so a game file comes back byte
/// for byte.
Bytes write_gxt2(const TextTable& t);

}  // namespace rk
