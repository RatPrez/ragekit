// ragekit - Scaleform movies (.gfx): the game's user interface (menus, the HUD, phone apps, the minimap). A .gfx is a
// Flash SWF with Scaleform's own signature and tags: "GFX" (or "CFX", the body zlib-compressed), a version, the length,
// the frame rectangle, rate and count, then the tags (a 10-bit code and a length each).
//
// ragekit keeps every tag's bytes as they are, so an uncompressed movie is written back byte for byte; tools read the
// tags they need (the exported symbols, the images a movie imports from texture dictionaries ...) from the payloads.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "types.h"

namespace rk {

struct GfxTag {
    uint16_t code = 0;        // 0 End, 1 ShowFrame, 9 SetBackgroundColor, 39 DefineSprite, 56 ExportAssets, 57 ImportAssets,
                              // 82 DoABC; Scaleform's own from 1000 (1001 ExporterInfo, 1003 DefineExternalImage ...)
    bool long_form = false;   // the length was written in the 6-byte form (some tags always are, even when short)
    Bytes payload;
};

struct GfxMovie {
    bool compressed = false;  // "CFX": the body after the 8-byte header is zlib-compressed
    uint8_t version = 8;
    Bytes frame;              // the frame rectangle as stored (bit-packed), frame rate (8.8) and frame count
    std::vector<GfxTag> tags;

    float frame_rate() const;
    uint16_t frame_count() const;
    /// The names ExportAssets tags give the movie's symbols, in order.
    std::vector<std::string> exports() const;
};

bool is_gfx(std::span<const uint8_t> file);
/// Throws rk::Error for a file that is not a movie or whose tags run past it.
GfxMovie read_gfx(std::span<const uint8_t> file);
/// Uncompressed movies come back byte for byte; a compressed one is compressed anew (the same movie, other bytes).
Bytes write_gfx(const GfxMovie& m);

}  // namespace rk
