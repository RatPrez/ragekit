// ragekit - the game's cache files (`gta5_cache_y.dat` and the packs' `*_cache_y.dat` / `*_cache_y_bank.dat`): what the
// game would otherwise compute when it loads, the map data store (every ymap's extents and parent), the interior
// proxies (every interior's placement) and the bounds store (every static collision file's extents and layer).
//
// The file is a 100-byte version area ("[VERSION]" and the version), then text lines (the file dates, the <module>
// markers) and, after a line naming one (fwMapDataStore, CInteriorProxy, BoundsStore), a module's binary payload: its
// length (u32) and that many bytes. A NUL ends the file. ragekit keeps every line and payload as stored, so a file comes
// back byte for byte; the views decode the payloads.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "types.h"

namespace rk {

struct CacheMapNode {          // 64 bytes: a ymap of the map data store
    uint32_t name = 0, parent = 0, content_flags = 0;
    Vec3 streaming_min, streaming_max, entities_min, entities_max;
    uint8_t flags[4] = {};     // CodeWalker's Unk1..4 (GUESS: HD / LOD / SLOD flags)
};
struct CacheInteriorProxy {    // 104 bytes
    uint32_t unk00 = 0, unk04 = 0, unk08 = 0;
    uint32_t name = 0, parent = 0;   // the MLO archetype and the ymap that places it
    Vec3 position;
    Quat orientation;
    Vec3 bb_min, bb_max;
    uint64_t unk50 = 0, unk58 = 0, unk60 = 0, unk68 = 0;
};
struct CacheBoundsItem {       // 32 bytes
    uint32_t name = 0;
    Vec3 min, max;
    uint32_t layer = 0;
};
struct CacheFileDate {         // a line of <fileDates>: "<hash> <timestamp> <id>"
    uint32_t name = 0;
    int64_t timestamp = 0;
    uint32_t id = 0;
};

struct CacheDat {
    Bytes version_area;        // the first 100 bytes as stored
    struct Part {
        std::string line;      // a text line (without its '\n'), or the name of the module that follows
        bool module = false;
        Bytes payload;         // a module's bytes
    };
    std::vector<Part> parts;
    Bytes tail;                // from the terminating NUL on

    std::string version() const;
    std::vector<CacheFileDate> file_dates() const;
    std::vector<CacheMapNode> map_nodes() const;
    std::vector<CacheInteriorProxy> interior_proxies() const;
    std::vector<CacheBoundsItem> bounds() const;
};

bool is_cache_dat(std::span<const uint8_t> file);
CacheDat read_cache_dat(std::span<const uint8_t> file);
Bytes write_cache_dat(const CacheDat& c);

}  // namespace rk
