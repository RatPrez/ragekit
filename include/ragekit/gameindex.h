// ragekit - an index of a GTA V install (Legacy or Enhanced), so tools can find and pull game assets by name.
//
// open() scans every archive (common.rpf, x64*.rpf, update/update.rpf, update2.rpf, every
// update/x64/dlcpacks/<pack>/dlc*.rpf, and all archives nested in them), records every file entry
// and every archetype of every .ytyp, and caches the result under %USERPROFILE%\.ragekit\index.
// A cached index loads in a fraction of a second; the first scan takes a minute or two.
//
// Load order: base archives < update.rpf < update2.rpf < the DLC packs listed in
// update.rpf/common/data/dlclist.xml, sorted by the <order> of their setup2.xml (ties keep the
// dlclist position); update.rpf/dlc_patch/<pack> sits just above the pack it patches. Packs the
// list does not name are never mounted: their files stay in the index but never win a lookup.
// This is rat-tooling's GameLoadOrder.Rank. When several files carry the same name the highest
// rank wins; at equal rank the one scanned last.
//
// Names are hashes (joaat of the lowercased name); the index keeps the file name stems it saw
// as a hash -> string table, so name() can print most archetypes (their drawable has the same name).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "game.h"
#include "types.h"
#include "rpf.h"
#include "map.h"

namespace rk {

/// One file entry (not a directory) of one archive.
struct IndexedFile {
    uint32_t name_hash = 0;   // joaat_lower of the file name without its last extension
    uint32_t archive = 0;     // index into GameIndex::archives()
    uint32_t entry = 0;       // entry index inside that archive's table
    uint32_t dir = 0;         // index of the directory (see GameIndex::path)
    uint32_t size = 0;        // extracted size in bytes
    uint32_t name_id = 0;     // row of this file's own name stem in the name table (two stems can share a hash)
    uint16_t ext = 0;         // index into GameIndex::extensions()
    uint16_t rank = 0;        // index into GameIndex::ranks(); higher = loaded later
};

/// An archive: a file on disk or an .rpf nested in another archive.
struct IndexedArchive {
    std::string path;         // '/' separated from the game folder, original case: "x64c.rpf/levels/.../props.rpf"
    int32_t parent = -1;      // archive holding this one (-1: a file on disk)
    uint32_t parent_entry = 0;
    uint32_t file_count = 0;
};

/// Where in the load order a file sits. A greater (tier, order, index, patch) is loaded later.
struct LoadRank {
    int32_t tier = 0;         // -1 pack the list never mounts, 0 base, 1 update.rpf, 2 update2.rpf, 3 DLC pack
    int32_t order = 0;        // setup2.xml <order> of the pack (tier 3)
    int32_t index = 0;        // position among the mounted packs (tier 3)
    int32_t patch = 0;        // 1: update.rpf/dlc_patch/<pack> (the title update's copy of the pack)
    std::string dlc;          // pack name (tier -1 and 3)
    bool active() const { return tier >= 0; }
    std::string label() const;
};

struct IndexedArchetype {
    enum Kind : uint8_t { Base = 0, Time = 1, Mlo = 2 };
    uint32_t name = 0;                 // joaat_lower of the archetype name
    uint32_t asset_name = 0;
    uint32_t texture_dictionary = 0;
    uint32_t drawable_dictionary = 0;
    uint32_t physics_dictionary = 0;
    uint32_t clip_dictionary = 0;
    uint32_t flags = 0;
    uint32_t ytyp_name = 0;            // the name the ytyp itself carries
    uint32_t ytyp_file = 0;            // index into GameIndex::files() of the .ytyp it comes from
    float    lod_dist = 0;
    Vec3     bb_min, bb_max;
    uint8_t  kind = Base;
    int8_t   asset_type = 2;           // 1 fragment, 2 drawable, 3 dictionary, 4 assetless, 0 older 128 byte structure (no asset fields)
    uint16_t rank = 0;                 // rank of the ytyp's file
};

/// What an archetype's model resolves to.
struct ResolvedDrawable {
    enum Type { None, Ydr, Yft, Ydd } type = None;
    std::optional<IndexedFile> file;   // the .ydr / .yft / .ydd that holds it
    uint32_t drawable_name = 0;        // Ydd: the drawable's key inside the dictionary
};

struct GameIndexOptions {
    bool rebuild = false;                                  // ignore (and replace) a cached index
    bool use_cache = true;                                 // false: neither read nor write the cache
    std::string cache_folder;                              // default %USERPROFILE%\.ragekit\index
    int threads = 0;                                       // scan threads, 0 = automatic
    std::function<void(const std::string&)> progress;
};

struct GameIndexInfo {
    std::string game_dir;
    Game game = Game::Legacy;          // the edition of the install (GTA5_Enhanced.exe: Enhanced)
    std::string exe_version;           // from versioninfo.txt when present
    std::string key;                   // cache key (hex)
    std::string cache_path;
    bool from_cache = false;
    double seconds = 0;                // open() wall time
    double scan_seconds = 0;           // of the scan that built the index (stored in the cache)
    uint64_t cache_bytes = 0;
    std::vector<std::string> warnings; // archives/ytyps that could not be read
};

class GameIndex {
public:
    /// Cache file format version (bump when the layout changes; older caches are rebuilt).
    static constexpr uint32_t kFormat = 1;

    /// Opens (or builds) the index of an install. Throws rk::Error (no keys, not a game folder).
    static GameIndex open(const std::string& game_dir, const GameIndexOptions& opt = {});
    /// The install of default_game() (game.h) in the usual place, empty if there is none.
    static std::string default_game_dir();
    /// The install of an edition in the usual place, empty if there is none: $RAGEKIT_GAME (Legacy) or
    /// $RAGEKIT_GAME_ENHANCED (Enhanced), then the Rockstar Games, Steam and Epic folders.
    static std::string default_game_dir(Game g);
    /// The edition of an install folder: Enhanced when it holds GTA5_Enhanced.exe and no GTA5.exe.
    static Game game_of_dir(const std::string& game_dir);

    const GameIndexInfo& info() const;
    const std::vector<IndexedArchive>& archives() const;
    const std::vector<IndexedFile>& files() const;           // sorted by (name_hash, ext, rank)
    const std::vector<IndexedArchetype>& archetypes() const; // sorted by (name, rank)
    const std::vector<std::string>& extensions() const;      // ".ydr", ".ytd", ...
    const std::vector<LoadRank>& ranks() const;              // ascending

    // --- names ---------------------------------------------------------------------------
    /// The string for a hash if some file name had it; else "hash_XXXXXXXX".
    std::string name(uint32_t hash) const;
    bool known_name(uint32_t hash, std::string* out = nullptr) const;

    // --- files ---------------------------------------------------------------------------
    std::string file_name(const IndexedFile& f) const;       // "v_ilev_mm_door.ydr" (lower case)
    std::string path(const IndexedFile& f) const;            // archive path + directory + file name
    std::string archive_path(const IndexedFile& f) const;
    const std::string& extension(const IndexedFile& f) const;
    const LoadRank& rank(const IndexedFile& f) const;
    /// Every copy of the file name, the one the game uses last. `ext` like ".ydr" (any case).
    std::vector<IndexedFile> versions(uint32_t name_hash, const std::string& ext) const;
    /// The copy the game uses; none if there is no copy or only copies of packs that are never mounted.
    std::optional<IndexedFile> resolve_file(uint32_t name_hash, const std::string& ext) const;
    std::optional<IndexedFile> resolve_file(const std::string& name, const std::string& ext) const;
    /// Every file with this stem (any extension), the winner of each extension.
    std::vector<IndexedFile> resolve_stem(uint32_t name_hash) const;
    /// Files whose stem contains `substring` (case-insensitive), winners only. `ext` empty = any.
    std::vector<IndexedFile> find_files(const std::string& substring, const std::string& ext = {}, size_t limit = 100) const;

    /// The .ydr or .yft of a name (the .ydr first, as CodeWalker); none if neither exists.
    ResolvedDrawable resolve_drawable(uint32_t name_hash) const;
    ResolvedDrawable resolve_drawable(const std::string& name) const;
    /// What the archetype shows: its drawable dictionary entry, else the .ydr / .yft named by its asset
    /// name (the archetype name when it has none; CodeWalker does the same), failing that by its name.
    ResolvedDrawable resolve_drawable(const IndexedArchetype& a) const;
    std::optional<IndexedFile> resolve_txd(uint32_t name_hash) const;
    std::optional<IndexedFile> resolve_txd(const std::string& name) const;
    /// Texture dictionaries a txd falls back on, nearest first (gtxd.ymt / gtxd.meta parents).
    std::vector<uint32_t> txd_parents(uint32_t txd_hash) const;

    // --- archetypes ----------------------------------------------------------------------
    /// The definition the game uses (the newest ytyp), none if the name is unknown.
    std::optional<IndexedArchetype> archetype(uint32_t name_hash) const;
    std::optional<IndexedArchetype> archetype(const std::string& name) const;
    /// Every definition of the name, oldest first.
    std::vector<IndexedArchetype> archetype_versions(uint32_t name_hash) const;
    /// Archetypes whose name contains `substring` (case-insensitive), winners only, by name.
    std::vector<IndexedArchetype> find_archetypes(const std::string& substring, size_t limit = 100) const;
    /// The .ytyp file an archetype was read from.
    const IndexedFile& ytyp_of(const IndexedArchetype& a) const;
    /// Number of archetypes read from one .ytyp file (index into files()).
    size_t archetype_count(uint32_t ytyp_file) const;

    // --- bytes ---------------------------------------------------------------------------
    /// The file as a standalone file (resources with their RSC7 header), read through the Rpf reader.
    Bytes extract(const IndexedFile& f) const;

    struct Impl;
private:
    std::shared_ptr<Impl> d_;
};

} // namespace rk
