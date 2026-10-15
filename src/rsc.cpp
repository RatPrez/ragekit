#include "ragekit/rsc.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <numeric>
#include "miniz.h"

namespace rk::rsc {

namespace {

struct Field { uint8_t pos, bits; };
constexpr Field kFields[9] = {{4, 1}, {5, 2}, {7, 4}, {11, 6}, {17, 7}, {24, 1}, {25, 1}, {26, 1}, {27, 1}};

std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llX", (unsigned long long)v);
    return b;
}

uint64_t align16(uint64_t v) { return (v + 15) & ~uint64_t(15); }

struct Page { int cls; uint64_t used; };
struct Packing {
    uint32_t shift = 0;
    std::vector<Page> pages;
    std::vector<std::pair<size_t, uint64_t>> at;  // per item: page, offset
    uint64_t total = UINT64_MAX;
};

enum class Open { Tightest, LargestNeeded, Largest };

// Best fit, largest item first (the root, when there is one, opens the first page); every page then shrinks to the smallest class that
// holds it, the root's staying the largest. A new page opens at the tightest class that holds its item, the largest class any item
// needs, or the largest class with a page left (the fewest pages).
bool pack(const std::vector<uint64_t>& sizes, bool root, uint32_t shift, Open open, Packing& out) {
    const auto cap = [&](int cls) { return (uint64_t(0x200) << shift) << (8 - cls); };
    int smallest = 8;
    while (smallest > 0 && cap(smallest) < kMinPageSize) smallest--;
    const auto fit = [&](uint64_t size) {
        for (int c = smallest; c >= 0; c--)
            if (cap(c) >= size) return c;
        return -1;
    };
    int top = smallest;
    for (uint64_t s : sizes) {
        const int c = fit(s);
        if (c < 0) return false;
        top = std::min(top, c);
    }
    std::vector<size_t> order(sizes.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin() + (root ? 1 : 0), order.end(), [&](size_t a, size_t b) { return sizes[a] > sizes[b]; });

    Packing p;
    p.shift = shift;
    p.at.resize(sizes.size());
    std::array<uint32_t, 9> count{};
    for (size_t k = 0; k < order.size(); k++) {
        const size_t i = order[k];
        const uint64_t s = sizes[i];
        size_t best = SIZE_MAX;
        uint64_t best_left = UINT64_MAX;
        if (!(root && k == 0))
            for (size_t pi = 0; pi < p.pages.size(); pi++) {
                const uint64_t end = align16(p.pages[pi].used) + s, c = cap(p.pages[pi].cls);
                if (end <= c && c - end < best_left) { best = pi; best_left = c - end; }
            }
        if (best == SIZE_MAX) {
            int c = open == Open::Tightest && !(root && k == 0) ? fit(s) : top;
            if (open == Open::Largest) {
                c = 0;
                while (c <= fit(s) && count[(size_t)c] >= PageFlags::limit(c)) c++;
                if (c > fit(s)) return false;
            } else {
                const int largest = root ? top : 0;
                while (c >= largest && count[(size_t)c] >= PageFlags::limit(c)) c--;
                if (c < largest) return false;
            }
            count[(size_t)c]++;
            p.pages.push_back({c, 0});
            best = p.pages.size() - 1;
        }
        Page& pg = p.pages[best];
        p.at[i] = {best, align16(pg.used)};
        pg.used = align16(pg.used) + s;
    }

    const auto shrink = [&](Page& pg, int to) {
        for (int c = to; c > pg.cls; c--)
            if (cap(c) >= pg.used && count[(size_t)c] < PageFlags::limit(c)) {
                count[(size_t)pg.cls]--;
                count[(size_t)c]++;
                pg.cls = c;
                return;
            }
    };
    std::vector<size_t> by_use(p.pages.size());
    std::iota(by_use.begin(), by_use.end(), size_t(0));
    std::stable_sort(by_use.begin(), by_use.end(), [&](size_t a, size_t b) { return p.pages[a].used < p.pages[b].used; });
    for (size_t pi : by_use)
        if (!(root && pi == 0)) shrink(p.pages[pi], smallest);
    if (root) {
        int to = smallest;
        for (size_t pi = 1; pi < p.pages.size(); pi++) to = std::min(to, p.pages[pi].cls);
        shrink(p.pages[0], to);
    }
    p.total = 0;
    for (const Page& pg : p.pages) p.total += cap(pg.cls);
    out = std::move(p);
    return true;
}

}  // namespace

std::array<uint32_t, 9> PageFlags::counts() const {
    std::array<uint32_t, 9> c{};
    for (int i = 0; i < 9; i++) c[(size_t)i] = (value >> kFields[i].pos) & ((1u << kFields[i].bits) - 1);
    return c;
}

std::array<uint32_t, 9> PageFlags::sizes() const {
    std::array<uint32_t, 9> s{};
    for (int i = 0; i < 9; i++) s[(size_t)i] = base_size() << (8 - i);
    return s;
}

uint32_t PageFlags::page_count() const {
    const auto c = counts();
    return std::accumulate(c.begin(), c.end(), 0u);
}

uint64_t PageFlags::total_size() const {
    const auto c = counts();
    uint64_t t = 0;
    for (int i = 0; i < 9; i++) t += (uint64_t)c[(size_t)i] * ((uint64_t)base_size() << (8 - i));
    return t;
}

uint32_t PageFlags::limit(int cls) { return (1u << kFields[cls].bits) - 1; }

PageFlags PageFlags::make(uint32_t shift, const std::array<uint32_t, 9>& counts) {
    PageFlags f;
    f.value = shift & 0xF;
    for (int i = 0; i < 9; i++) {
        if (counts[(size_t)i] > limit(i)) throw Error("page class " + std::to_string(i) + " holds at most " + std::to_string(limit(i)) + " pages");
        f.value |= counts[(size_t)i] << kFields[i].pos;
    }
    return f;
}

bool Resource::is_resource(std::span<const uint8_t> file) {
    uint32_t m = 0;
    if (file.size() >= 16) std::memcpy(&m, file.data(), 4);
    return m == kMagic;
}

Resource Resource::load(std::span<const uint8_t> file) {
    if (!is_resource(file)) throw Error("not an RSC7 resource");
    Resource r;
    std::memcpy(&r.version, file.data() + 4, 4);
    std::memcpy(&r.system_flags.value, file.data() + 8, 4);
    std::memcpy(&r.graphics_flags.value, file.data() + 12, 4);
    const size_t sys = (size_t)r.system_flags.total_size(), gfx = (size_t)r.graphics_flags.total_size();
    Bytes body = inflate(file.subspan(16), sys + gfx);
    if (body.size() < sys) throw Error("resource body ends inside its system segment");
    body.resize(sys + gfx);
    r.system.assign(body.begin(), body.begin() + (ptrdiff_t)sys);
    r.graphics.assign(body.begin() + (ptrdiff_t)sys, body.end());
    return r;
}

Bytes Resource::save(bool compress) const {
    Bytes body(system.size() + graphics.size());
    std::copy(system.begin(), system.end(), body.begin());
    std::copy(graphics.begin(), graphics.end(), body.begin() + (ptrdiff_t)system.size());
    if (compress) body = deflate(body);
    const uint32_t v = (uint32_t)version;
    const uint32_t head[4] = {kMagic, v, (system_flags.value & 0x0FFFFFFF) | ((v >> 4 & 0xF) << 28), (graphics_flags.value & 0x0FFFFFFF) | ((v & 0xF) << 28)};
    Bytes out(16 + body.size());
    std::memcpy(out.data(), head, 16);
    std::copy(body.begin(), body.end(), out.begin() + 16);
    return out;
}

const uint8_t* Resource::at(uint64_t ptr, size_t need) const {
    const bool sys = in_system(ptr);
    if (!sys && !in_graphics(ptr)) throw Error("pointer " + hex(ptr) + " is in neither segment");
    const Bytes& seg = sys ? system : graphics;
    const uint64_t off = ptr - (sys ? kSystemBase : kGraphicsBase);
    if (off + need > seg.size())
        throw Error("pointer " + hex(ptr) + " + " + std::to_string(need) + " runs past the " + (sys ? "system" : "graphics") + " segment (" + std::to_string(seg.size()) + " bytes)");
    return seg.data() + off;
}

Bytes deflate(std::span<const uint8_t> data) {
    mz_stream z{};
    if (mz_deflateInit2(&z, MZ_BEST_COMPRESSION, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY) != MZ_OK) throw Error("deflate: init failed");
    Bytes out(mz_deflateBound(&z, (mz_ulong)data.size()));
    z.next_in = data.data();
    z.avail_in = (unsigned)data.size();
    z.next_out = out.data();
    z.avail_out = (unsigned)out.size();
    const int rc = mz_deflate(&z, MZ_FINISH);
    out.resize(z.total_out);
    mz_deflateEnd(&z);
    if (rc != MZ_STREAM_END) throw Error("deflate failed");
    return out;
}

Bytes inflate(std::span<const uint8_t> data, size_t expected_size) {
    mz_stream z{};
    if (mz_inflateInit2(&z, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) throw Error("inflate: init failed");
    Bytes out(expected_size);
    z.next_in = data.data();
    z.avail_in = (unsigned)data.size();
    z.next_out = out.data();
    z.avail_out = (unsigned)out.size();
    const int rc = mz_inflate(&z, MZ_FINISH);
    out.resize(z.total_out);
    mz_inflateEnd(&z);
    if (rc != MZ_STREAM_END && rc != MZ_BUF_ERROR && rc != MZ_OK) throw Error("inflate: corrupt deflate stream");
    return out;
}

Bytes read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw Error("cannot open " + path);
    Bytes b((size_t)f.tellg());
    f.seekg(0);
    f.read((char*)b.data(), (std::streamsize)b.size());
    return b;
}

void write_file(const std::string& path, std::span<const uint8_t> data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw Error("cannot write " + path);
    f.write((const char*)data.data(), (std::streamsize)data.size());
}

}  // namespace rk::rsc

namespace rk {

BlockId Builder::alloc(Segment seg, size_t size, size_t) {
    blocks_.push_back(Block{seg, Bytes(size), {}, 0});
    return BlockId{(uint32_t)(blocks_.size() - 1)};
}

void Builder::pointer(BlockId from, size_t at, BlockId to, uint64_t addend) {
    if (!to.valid()) return;
    if (at + 8 > blocks_[from.index].bytes.size()) throw Error("Builder: a pointer outside its block");
    blocks_[from.index].fixups.push_back({at, to, addend});
}

void Builder::put(BlockId b, size_t at, const void* src, size_t n) {
    Bytes& bytes = blocks_[b.index].bytes;
    if (at + n > bytes.size()) throw Error("Builder: " + std::to_string(n) + " bytes at " + std::to_string(at) + " overrun a block of " + std::to_string(bytes.size()));
    std::memcpy(bytes.data() + at, src, n);
}

BlockId Builder::string(const std::string& s) {
    const BlockId b = alloc(Segment::System, s.size() + 1);
    put(b, 0, s.data(), s.size());
    return b;
}

BlockId Builder::array(Segment seg, const void* src, size_t bytes, size_t align) {
    if (!bytes) return BlockId{};
    const BlockId b = alloc(seg, bytes, align);
    put(b, 0, src, bytes);
    return b;
}

BlockId Builder::alloc_pages_info() {
    pages_info_ = alloc(Segment::System, 16 + 8);
    return pages_info_;
}

rsc::PageFlags Builder::layout(Segment seg, uint32_t max_pages, uint64_t& total) {
    std::vector<uint32_t> ids;
    std::vector<uint64_t> sizes;
    for (uint32_t i = 0; i < blocks_.size(); i++)
        if (blocks_[i].seg == seg) {
            ids.push_back(i);
            sizes.push_back(std::max<uint64_t>(blocks_[i].bytes.size(), 1));
        }
    total = 0;
    if (ids.empty()) return {};
    const bool sys = seg == Segment::System;
    rsc::Packing best;
    for (uint32_t shift = 0; shift < 16; shift++)
        for (rsc::Open open : {rsc::Open::Tightest, rsc::Open::LargestNeeded, rsc::Open::Largest}) {
            rsc::Packing p;
            if (!rsc::pack(sizes, sys, shift, open, p) || p.pages.size() > max_pages) continue;
            if (p.total < best.total || (p.total == best.total && p.pages.size() < best.pages.size())) best = std::move(p);
        }
    if (best.total == UINT64_MAX) throw Error("Builder: the blocks fit in no page layout");

    std::vector<size_t> order(best.pages.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return best.pages[a].cls < best.pages[b].cls; });
    std::vector<uint64_t> page_at(best.pages.size());
    std::array<uint32_t, 9> counts{};
    const uint64_t base = uint64_t(0x200) << best.shift;
    for (size_t pi : order) {
        page_at[pi] = total;
        total += base << (8 - best.pages[pi].cls);
        counts[(size_t)best.pages[pi].cls]++;
    }
    const uint64_t seg_base = sys ? rsc::kSystemBase : rsc::kGraphicsBase;
    for (size_t k = 0; k < ids.size(); k++) blocks_[ids[k]].position = seg_base + page_at[best.at[k].first] + best.at[k].second;
    return rsc::PageFlags::make(best.shift, counts);
}

Bytes Builder::build(int version, bool compress) {
    if (blocks_.empty() || blocks_[0].seg != Segment::System) throw Error("Builder: the first block must be the system root");
    rsc::Resource res;
    res.version = version;
    uint64_t sys = 0, gfx = 0;
    for (uint32_t pages = 1;;) {
        if (pages_info_.valid()) blocks_[pages_info_.index].bytes.assign(16 + 8 * (size_t)pages, 0);
        res.system_flags = layout(Segment::System, rsc::kMaxPages, sys);
        res.graphics_flags = layout(Segment::Graphics, rsc::kMaxPages - res.system_flags.page_count(), gfx);
        const uint32_t used = res.system_flags.page_count() + res.graphics_flags.page_count();
        if (!pages_info_.valid() || used <= pages) break;
        pages = used;
    }
    res.system.assign((size_t)sys, 0);
    res.graphics.assign((size_t)gfx, 0);
    for (Block& b : blocks_) {
        for (const Fixup& f : b.fixups) {
            const uint64_t v = blocks_[f.to.index].position + f.addend;
            std::memcpy(b.bytes.data() + f.at, &v, 8);
        }
        const bool in_sys = b.seg == Segment::System;
        std::copy(b.bytes.begin(), b.bytes.end(), (in_sys ? res.system : res.graphics).begin() + (ptrdiff_t)(b.position - (in_sys ? rsc::kSystemBase : rsc::kGraphicsBase)));
    }
    if (pages_info_.valid()) {
        uint8_t* p = res.system.data() + (blocks_[pages_info_.index].position - rsc::kSystemBase);
        p[8] = (uint8_t)res.system_flags.page_count();
        p[9] = (uint8_t)res.graphics_flags.page_count();
    }
    return res.save(compress);
}

std::string Reader::str(uint64_t ptr) const {
    std::string s;
    if (!ptr) return s;
    for (char c; (c = (char)u8(ptr++)) != 0;) {
        s.push_back(c);
        if (s.size() > 4096) throw Error("unterminated string in resource");
    }
    return s;
}

size_t Reader::remaining(uint64_t ptr) const {
    const bool sys = res_.in_system(ptr);
    if (!sys && !res_.in_graphics(ptr)) return 0;
    const Bytes& seg = sys ? res_.system : res_.graphics;
    const uint64_t off = ptr - (sys ? rsc::kSystemBase : rsc::kGraphicsBase);
    return off < seg.size() ? seg.size() - (size_t)off : 0;
}

}  // namespace rk
