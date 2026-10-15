// ragekit - the streaming manifest `_manifest.ymf` (a PSO whose root is CPackFileMetaData).
//
// A resource that ships a custom interior must list its ytyp in itypDependencies_2 and its interior
// collision (ybn) in Interiors, or the client crashes when the player gets close. This is the typed
// reader/writer for that file. Names are stored as joaat hashes (the file stores nothing else); use
// name_hash("rat_apt01") to build them from strings.
//
// write_ymf produces what CodeWalker's XML -> PSO conversion produces: sections PSIN, PMAP, PSCH only,
// blocks in creation order without padding, schema infos only for the types in use.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "types.h"
#include "error.h"

namespace rk {

/// joaat of the lower-cased string, which is how the manifest stores every name.
uint32_t manifest_hash(const std::string& name);

namespace manifest_flags {
constexpr uint32_t kInteriorData = 1;     // manifestFlags INTERIOR_DATA (enum value 0 -> bit 0)
}

struct ManifestMapDataGroup {            // CMapDataGroup (56 bytes)
    uint32_t name = 0;
    std::vector<uint32_t> bounds;        // ybn names
    uint32_t flags = 0;                  // bit 0 TIME_DEPENDENT, bit 1 WEATHER_DEPENDENT
    std::vector<uint32_t> weather_types;
    uint32_t hours_on_off = 0;
};
struct ManifestHdTxd {                   // CHDTxdAssetBinding (132 bytes)
    int32_t asset_type = 0;              // 0 AT_TXD, 1 AT_DRB, 2 AT_DWD, 3 AT_FRG
    std::string target_asset;            // up to 63 characters
    std::string hd_txd;
};
struct ManifestImapDependency {          // CImapDependency (12 bytes)
    uint32_t imap_name = 0, ityp_name = 0, pack_file_name = 0;
};
struct ManifestDependencies {            // CImapDependencies / CItypDependencies (24 bytes)
    uint32_t name = 0;                   // imapName / itypName
    uint32_t flags = 0;                  // manifestFlags (kInteriorData)
    std::vector<uint32_t> deps;          // itypDepArray
};
struct ManifestInterior {                // CInteriorBoundsFiles (24 bytes)
    uint32_t name = 0;
    std::vector<uint32_t> bounds;        // ybn names
};

struct Manifest {
    std::vector<ManifestMapDataGroup>  map_data_groups;
    std::vector<ManifestHdTxd>         hd_txd_bindings;
    std::vector<ManifestImapDependency> imap_dependencies;
    std::vector<ManifestDependencies>  imap_dependencies_2;
    std::vector<ManifestDependencies>  ityp_dependencies_2;
    std::vector<ManifestInterior>      interiors;

    /// Convenience for the usual interior entries (names are hashed with manifest_hash):
    /// itypDependencies_2 { ityp, INTERIOR_DATA, deps }.
    void add_ityp_dependency(const std::string& ityp, const std::vector<std::string>& deps = {}, uint32_t flags = manifest_flags::kInteriorData);
    /// imapDependencies_2 { imap, INTERIOR_DATA, ityps }.
    void add_imap_dependency(const std::string& imap, const std::vector<std::string>& ityps, uint32_t flags = manifest_flags::kInteriorData);
    /// Interiors { name, ybn names }.
    void add_interior(const std::string& name, const std::vector<std::string>& ybns);
};

/// Parses a `_manifest.ymf`. Throws rk::Error for anything that is not a PSO with a CPackFileMetaData root
/// (RBF and RSC7 manifests exist in the game archives; they are not handled).
Manifest read_ymf(std::span<const uint8_t> file);
/// Builds the file.
Bytes write_ymf(const Manifest& m);

} // namespace rk
