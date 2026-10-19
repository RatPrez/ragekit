// ragekit - RPF7 archives (GTA V).
//
// An RPF is a table of entries (directories, binary files, resource files) followed by file
// data in 512 byte blocks. Nested .rpf entries are archives inside the archive. Header, entry
// table and names table, and individual files, may be encrypted with AES or "NG"; the keys come
// from the game executable (see RpfKeys). Unencrypted archives (FiveM resource.rpf, OpenIV
// style) need no keys.
//
// RpfBuilder writes them: plain, OPEN, AES or NG, for GTA V Legacy and Enhanced alike (they share the format and keys).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "types.h"

namespace rk {

/// Key material derived from the game executable. NEVER commit these files (they are the
/// game's keys): the default folder is %USERPROFILE%\.ragekit\keys.
struct RpfKeys {
    Bytes aes;                       // 32 bytes (gtav_aes_key.dat)
    Bytes ng_keys;                   // 101 x 272 bytes (gtav_ng_key.dat)
    std::vector<uint32_t> ng_tables; // 17 rounds x 16 x 256 words (gtav_ng_decrypt_tables.dat)
    Bytes hash_lut;                  // 256 bytes (gtav_hash_lut.dat): file name hash used to pick the NG key
    Bytes awc;                       // 16 bytes (gtav_awc_key.dat): the XXTEA key of encrypted .awc audio (from magic.dat)

    bool has_aes() const { return aes.size() == 32; }
    bool has_awc() const { return awc.size() == 16; }
    bool has_ng() const { return ng_keys.size() == 101 * 272 && ng_tables.size() == 17 * 16 * 256 && hash_lut.size() == 256; }

    /// %USERPROFILE%\.ragekit\keys (or $HOME/.ragekit/keys).
    static std::string default_folder();
    /// Reads the CodeWalker cache files that exist in `folder`; missing files stay empty.
    static RpfKeys load(const std::string& folder);
    /// Writes the files CodeWalker's GTA5Keys.SaveToPath writes (same names and layout), minus
    /// CodeWalker's NG encryption tables/LUTs (ragekit derives its NG encryption from the decrypt tables).
    void save(const std::string& folder) const;
    /// Searches the executable for the keys by SHA1 hash, as CodeWalker does (every 8-byte
    /// aligned offset is hashed; a few seconds). Current executables hold only the AES key and the
    /// hash LUT: the NG keys and tables then come from `magic_path`, CodeWalker's embedded
    /// resource magic.dat (CodeWalker.Core/Resources), which is decoded with the AES key.
    /// Throws if the AES key is not found.
    static RpfKeys generate(const std::string& exe_path, const std::function<void(const std::string&)>& progress = {},
                            const std::string& magic_path = {});
    /// Keys from default_folder(), loaded once; empty keys (has_aes() false) if there are none.
    static std::shared_ptr<const RpfKeys> default_keys();
};

enum class RpfEncryption : uint32_t { None = 0, Open = 0x4E45504F, Aes = 0x0FFFFFF9, Ng = 0x0FEFFFFF };

struct RpfEntry {
    enum class Kind { Directory, Binary, Resource };
    Kind kind = Kind::Directory;
    std::string name;   // as stored (original case)
    std::string path;   // '/' separated from the archive root, original case; "" for the root
    int32_t parent = -1;
    uint32_t name_offset = 0;
    // Directory: children are entries [first_child, first_child + child_count) of the table.
    uint32_t first_child = 0, child_count = 0;
    // Files. `offset_blocks` is in 512 byte blocks from the archive start.
    uint32_t offset_blocks = 0;
    uint32_t stored_size = 0;       // bytes on disk (0 for a binary file means stored uncompressed, see uncompressed_size)
    uint32_t uncompressed_size = 0; // binary files
    bool encrypted = false;         // binary: the table says so; resource: only .ysc is
    uint32_t system_flags = 0, graphics_flags = 0; // resources

    bool is_directory() const { return kind == Kind::Directory; }
    bool is_resource() const { return kind == Kind::Resource; }
    bool is_binary() const { return kind == Kind::Binary; }
    bool is_archive() const; // a binary file named *.rpf
    /// Binary: the deflate flag (stored_size > 0). Resources are always deflated.
    bool compressed() const { return kind == Kind::Binary && stored_size > 0; }
    /// Size of the extracted data: binary = uncompressed bytes; resource = system + graphics.
    uint64_t size() const;
    /// Resource version as the game numbers it ((system nibble << 4) + graphics nibble).
    uint32_t resource_version() const { return ((system_flags >> 28) << 4) + (graphics_flags >> 28); }
    uint64_t system_size() const;
    uint64_t graphics_size() const;
};

class Rpf {
public:
    /// Opens an archive on disk. `keys` defaults to RpfKeys::default_keys(); they are only
    /// consulted when something is encrypted. Throws rk::Error.
    static Rpf open(const std::string& path, std::shared_ptr<const RpfKeys> keys = {});
    /// Opens an archive held in memory. `name` feeds the NG key choice, so keep the real name.
    static Rpf open_memory(Bytes data, const std::string& name, std::shared_ptr<const RpfKeys> keys = {});

    const std::string& name() const;
    uint64_t byte_size() const;        // size of this archive's data
    RpfEncryption encryption() const;
    uint32_t names_length() const;
    const std::vector<RpfEntry>& entries() const; // entries()[0] is the root directory

    /// Child entries of a directory entry, in table order.
    std::vector<int> children(int dir) const;
    /// Finds an entry by path ('/' or '\', case-insensitive), "" is the root. -1 if absent.
    /// Does not descend into nested archives.
    int find(const std::string& path) const;

    /// Extracts a file. Binary: the decrypted, inflated bytes. Resource: a complete RSC7 file
    /// (16 byte header rebuilt from the entry's flags, then the deflate stream), which
    /// rsc::Resource::load takes. Throws rk::Error for directories and bad data.
    Bytes extract(int entry) const;
    Bytes extract(const RpfEntry& e) const;
    /// The bytes of a binary .rpf entry as stored (no inflate), e.g. to save a nested archive.
    Bytes extract_raw(int entry) const;
    /// The entry's data as stored but decrypted: a binary file still deflated when `compressed()`;
    /// a resource as a complete RSC7 file (same as extract). Used to copy entries without recompressing.
    Bytes extract_stored(int entry) const;
    /// Up to `length` of an entry's bytes as stored on disk (what extract_raw returns: still deflated and encrypted where
    /// the entry is, a resource with its on-disk 16-byte header), from `offset`: a header or a slice of a large entry
    /// without reading it whole. Fewer bytes come back where the entry ends; none past its end.
    Bytes read_stored(int entry, uint64_t offset, size_t length) const;
    /// The stored length of an entry (the size of extract_raw), 0 for a directory.
    uint64_t stored_length(int entry) const;
    /// Opens a nested archive entry without copying it.
    Rpf open_child(int entry) const;

    /// Resolves a path that may pass through nested archives ("update.rpf/x64/dlcpacks/x/dlc.rpf/...").
    /// Returns the archive holding it and the entry index, or {empty, -1}.
    struct Location { std::shared_ptr<Rpf> archive; int entry = -1; };
    Location resolve(const std::string& path) const;

    struct Impl;
private:
    std::shared_ptr<const Impl> d_;
};

// ---------------------------------------------------------------------------------------
// Writing

struct RpfWriteOptions {
    /// None and Open write plain tables (Open is what OpenIV/FiveM packs use); Aes and Ng encrypt the entry
    /// and name tables (files stay plain), Ng as the game's own archives are. Nested archives get the same.
    RpfEncryption encryption = RpfEncryption::Open;
    std::shared_ptr<const RpfKeys> keys; // for Aes and Ng; default RpfKeys::default_keys()
    /// The archive's file name ("dlc.rpf"): NG picks its key by the name (and the archive's size), so an NG archive
    /// must keep the name it was written for. save() takes it from the path when empty; build() needs it for NG.
    std::string name;
    bool compress = true;                // deflate binary files added from memory (not .rpf/.awc)
};

/// Builds RPF7 archives: add, replace, remove and rename entries, then write the whole archive
/// out. Layout follows CodeWalker: the table lists a directory's children contiguously, sorted by
/// name (ordinal), directories popped depth first; header, table and names pad to 512 byte blocks;
/// files follow in table order, each padded to a block. Paths use '/', lookups ignore case.
/// Copies share state: copy a builder with clone().
class RpfBuilder {
public:
    RpfBuilder();
    RpfBuilder clone() const;

    enum class Nested {
        Keep,    // nested archives that are not encrypted are copied byte for byte, encrypted ones are rewritten
        Rewrite, // every nested archive becomes a builder of its own (see nested())
    };
    /// Loads an archive's whole tree. Entries are copied lazily from `rpf` (which stays alive).
    static RpfBuilder from_archive(const Rpf& rpf, Nested mode = Nested::Keep);

    /// Intermediate directories are created. Adding over an existing file replaces it.
    void add_directory(const std::string& path);
    /// A binary file; `compress` = deflate it (not done for .rpf/.awc, nor when it does not shrink).
    void add_binary(const std::string& path, Bytes data, bool compress = true);
    /// A resource given as a complete RSC7 file: its system/graphics flags go into the entry.
    void add_resource(const std::string& path, Bytes rsc7_file);
    /// Picks add_resource for RSC7 data, else add_binary (an RPF7 .rpf is stored as is).
    void add_file(const std::string& path, Bytes data);
    /// A nested archive built by another builder (built with the same options when this one is).
    void add_archive(const std::string& path, RpfBuilder child);

    bool exists(const std::string& path) const;
    bool is_directory(const std::string& path) const;
    /// Removes a file or a directory with everything in it. False if absent.
    bool remove(const std::string& path);
    /// Renames the last component of `path` to `new_name` (no slash). Throws if the name is taken.
    bool rename(const std::string& path, const std::string& new_name);
    /// The builder of a nested archive loaded with Nested::Rewrite or added with add_archive; null otherwise.
    RpfBuilder* nested(const std::string& path);
    /// Paths of every entry, '/' separated, directories first within a folder as the table sorts them.
    std::vector<std::string> list() const;

    Bytes build(const RpfWriteOptions& opt = {}) const;
    void save(const std::string& path, const RpfWriteOptions& opt = {}) const;

    struct Node;
    const std::shared_ptr<Node>& root_node() const { return root_; }
private:
    std::shared_ptr<Node> root_;
};

} // namespace rk
