// ragekit - RSC7 resources: the container, its paged segments, and building / reading their blocks.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>
#include <string_view>
#include "error.h"
#include "game.h"
#include "types.h"

namespace rk::rsc {

constexpr uint32_t kMagic = 0x37435352;
constexpr uint64_t kSystemBase = 0x50000000;
constexpr uint64_t kGraphicsBase = 0x60000000;
constexpr uint32_t kMaxPages = 128;
constexpr uint64_t kMinPageSize = 0x2000;  // the game's own files never use a smaller page

enum Version : int {
    kYtd = 13, kYbn = 43, kYft = 162, kYdr = 165, kYdd = 165, kYcd = 46, kYmap = 2, kYtyp = 2,
    kYnv = 2, kYpt = 68, kYed = 25, kYld = 8, kYfd = 4,
    // GTA V Enhanced (game.h): the types whose layout differs carry versions of their own
    kYtdGen9 = 5, kYdrGen9 = 159, kYddGen9 = 159, kYftGen9 = 171, kYptGen9 = 71
};

/// The edition a resource version belongs to, for a file type (".ydr" or "ydr", any case): Enhanced for ytd 5, ydr and
/// ydd 159 (and the few ydr 154 of the game), yft 171, ypt 71; Legacy for everything else, which the editions share.
Game game_of(std::string_view type, int version);
/// The version a file type is written with for an edition (ytd 13 / 5 ...); 0 for a type ragekit does not know.
int version_for(std::string_view type, Game g);

// Page classes, largest first: class i holds pages of base << (8 - i).
struct PageFlags {
    uint32_t value = 0;
    uint32_t base_shift() const { return value & 0xF; }
    uint32_t base_size() const { return 0x200u << base_shift(); }
    std::array<uint32_t, 9> counts() const;
    std::array<uint32_t, 9> sizes() const;
    uint32_t page_count() const;
    uint64_t total_size() const;
    uint32_t version_nibble() const { return value >> 28; }
    static uint32_t limit(int cls);
    static PageFlags make(uint32_t shift, const std::array<uint32_t, 9>& counts);
};

struct Resource {
    int version = 0;
    PageFlags system_flags, graphics_flags;
    Bytes system, graphics;

    static bool is_resource(std::span<const uint8_t> file);
    static Resource load(std::span<const uint8_t> file);
    Bytes save(bool compress = true) const;

    /// Bytes at a game pointer, checked: throws when the pointer is in neither segment or `need` bytes run past it.
    const uint8_t* at(uint64_t ptr, size_t need) const;
    /// Bytes from a game pointer to the end of its segment; throws for a pointer in neither segment or past its end.
    uint64_t remaining_checked(uint64_t ptr) const;
    // A pointer is its segment's base plus an offset under 0x10000000 (the next base); its upper 32 bits are never set.
    bool in_system(uint64_t ptr) const { return (ptr >> 28) == (kSystemBase >> 28); }
    bool in_graphics(uint64_t ptr) const { return (ptr >> 28) == (kGraphicsBase >> 28); }
    /// The most a segment can hold: a longer one would have offsets reaching into the next segment's pointer range.
    static constexpr uint64_t kMaxSegmentSize = 0x10000000;
};

/// A pointer as "0x50001230", for messages.
std::string hex_ptr(uint64_t ptr);
Bytes deflate(std::span<const uint8_t> data);
/// Inflates a raw deflate stream of at most `expected_size` bytes (never more than the stream could hold, so a made-up
/// size costs no memory). A corrupt stream throws; a truncated one gives what it had.
Bytes inflate(std::span<const uint8_t> data, size_t expected_size);
Bytes read_file(const std::string& path);
void write_file(const std::string& path, std::span<const uint8_t> data);

}  // namespace rk::rsc

namespace rk {

enum class Segment { System, Graphics };

struct BlockId {
    uint32_t index = UINT32_MAX;
    bool valid() const { return index != UINT32_MAX; }
};

// The first system block is the root (system offset 0). build() lays the blocks out in the fewest bytes of pages.
class Builder {
public:
    /// The edition the structures are laid out for (game.h); the format writers read it.
    Game game() const { return game_; }
    void set_game(Game g) { game_ = g; }
    bool gen9() const { return game_ == Game::Enhanced; }

    BlockId alloc(Segment seg, size_t size, size_t align = 16);
    void resize(BlockId b, size_t size) { blocks_[b.index].bytes.resize(size); }
    uint8_t* data(BlockId b) { return blocks_[b.index].bytes.data(); }
    size_t size(BlockId b) const { return blocks_[b.index].bytes.size(); }
    Segment segment(BlockId b) const { return blocks_[b.index].seg; }

    void pointer(BlockId from, size_t at, BlockId to, uint64_t addend = 0);
    void put(BlockId b, size_t at, const void* src, size_t n);
    template <class T> void put(BlockId b, size_t at, const T& v) { put(b, at, &v, sizeof(T)); }

    BlockId string(const std::string& s);
    BlockId array(Segment seg, const void* src, size_t bytes, size_t align = 16);
    template <class T> BlockId array(Segment seg, const std::vector<T>& v, size_t align = 16) { return array(seg, v.data(), v.size() * sizeof(T), align); }

    // The root's page map (root + 8): 16 bytes and 8 per page, with the page counts at 8 and 9.
    BlockId alloc_pages_info();

    Bytes build(int version, bool compress = true);
    uint64_t position(BlockId b) const { return blocks_[b.index].position; }

private:
    struct Fixup { size_t at; BlockId to; uint64_t addend; };
    struct Block {
        Segment seg;
        Bytes bytes;
        std::vector<Fixup> fixups;
        uint64_t position = 0;
    };
    std::vector<Block> blocks_;
    BlockId pages_info_;
    Game game_ = Game::Legacy;
    rsc::PageFlags layout(Segment seg, uint32_t max_pages, uint64_t& total);
};

class BlockWriter {
public:
    BlockWriter(Builder& b, BlockId id, size_t pos = 0) : b_(b), id_(id), pos_(pos) {}
    size_t pos() const { return pos_; }
    void seek(size_t p) { pos_ = p; }
    BlockId block() const { return id_; }
    void bytes(const void* src, size_t n) { b_.put(id_, pos_, src, n); pos_ += n; }
    void u8(uint8_t v) { bytes(&v, 1); }
    void u16(uint16_t v) { bytes(&v, 2); }
    void u32(uint32_t v) { bytes(&v, 4); }
    void u64(uint64_t v) { bytes(&v, 8); }
    void i16(int16_t v) { bytes(&v, 2); }
    void f32(float v) { bytes(&v, 4); }
    void vec3(const Vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
    void vec4(const Vec4& v) { f32(v.x); f32(v.y); f32(v.z); f32(v.w); }
    void zero(size_t n) { pos_ += n; }
    void ptr(BlockId to, uint64_t addend = 0) { if (to.valid()) b_.pointer(id_, pos_, to, addend); pos_ += 8; }
    void pad16() { pos_ = (pos_ + 15) & ~size_t(15); }

private:
    Builder& b_;
    BlockId id_;
    size_t pos_;
};

class Reader {
public:
    explicit Reader(const rsc::Resource& r, Game g = Game::Legacy) : res_(r), game_(g) {}
    const rsc::Resource& resource() const { return res_; }
    /// The edition the resource was written for (the format readers set it from the version).
    Game game() const { return game_; }
    bool gen9() const { return game_ == Game::Enhanced; }

    template <class T> T at(uint64_t ptr) const { T v; std::memcpy(&v, res_.at(ptr, sizeof(T)), sizeof(T)); return v; }
    uint8_t u8(uint64_t p) const { return at<uint8_t>(p); }
    uint16_t u16(uint64_t p) const { return at<uint16_t>(p); }
    uint32_t u32(uint64_t p) const { return at<uint32_t>(p); }
    uint64_t u64(uint64_t p) const { return at<uint64_t>(p); }
    int16_t i16(uint64_t p) const { return at<int16_t>(p); }
    float f32(uint64_t p) const { return at<float>(p); }

    /// `count` items at ptr (none for a null pointer); checked against the segment before anything is allocated, so a
    /// count a malformed file made up costs nothing but the error.
    template <class T> std::vector<T> array(uint64_t ptr, size_t count) const {
        std::vector<T> v;
        if (!ptr || !count) return v;
        if (count > res_.remaining_checked(ptr) / sizeof(T))
            throw Error("array of " + std::to_string(count) + " x " + std::to_string(sizeof(T)) + " bytes at " + rsc::hex_ptr(ptr) + " runs past its segment");
        const uint8_t* p = res_.at(ptr, count * sizeof(T));
        v.resize(count);
        std::memcpy(v.data(), p, count * sizeof(T));
        return v;
    }
    Bytes bytes(uint64_t ptr, size_t count) const { return array<uint8_t>(ptr, count); }
    std::string str(uint64_t ptr) const;
    size_t remaining(uint64_t ptr) const;

    struct Cursor {
        const Reader& r;
        uint64_t pos;
        uint8_t u8() { auto v = r.u8(pos); pos += 1; return v; }
        uint16_t u16() { auto v = r.u16(pos); pos += 2; return v; }
        uint32_t u32() { auto v = r.u32(pos); pos += 4; return v; }
        uint64_t u64() { auto v = r.u64(pos); pos += 8; return v; }
        int16_t i16() { auto v = r.i16(pos); pos += 2; return v; }
        float f32() { auto v = r.f32(pos); pos += 4; return v; }
        Vec3 vec3() { Vec3 v{f32(), f32(), f32()}; return v; }
        Vec4 vec4() { Vec4 v; v.x = f32(); v.y = f32(); v.z = f32(); v.w = f32(); return v; }
        void skip(uint64_t n) { pos += n; }
    };
    Cursor cursor(uint64_t pos) const { return Cursor{*this, pos}; }

private:
    const rsc::Resource& res_;
    Game game_;
};

}  // namespace rk
