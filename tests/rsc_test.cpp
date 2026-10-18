// rsc_test: the RSC7 container and the page layout Builder makes (rsc.h).
//   - page flags encode and decode every class count, and refuse a count over its limit
//   - random block sets: the root at system offset 0, every block 16-aligned inside one page, no two overlapping, pages of at
//     least 8 KB, no more than 128 pages, the page map sized to the pages with their counts, contents and pointers intact
//   - how much of the pages the blocks fill
//   - inflate: the table-driven decoder on streams of every block type, and rsc::inflate against miniz on whole, damaged,
//     cut and capped streams
// Exit 0 = every check passed.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "ragekit/rsc.h"
#include "inflate_fast.h"
#include "miniz.h"

using namespace rk;

static int fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%-100s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

struct Rng {
    uint64_t s;
    uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(s >> 33); }
    uint32_t below(uint32_t n) { return next() % n; }
};

struct PageSpan { uint64_t at, size; };
static std::vector<PageSpan> pages_of(const rsc::PageFlags& f) {
    std::vector<PageSpan> v;
    uint64_t at = 0;
    const auto c = f.counts();
    const auto s = f.sizes();
    for (int i = 0; i < 9; i++)
        for (uint32_t k = 0; k < c[(size_t)i]; k++) { v.push_back({at, s[(size_t)i]}); at += s[(size_t)i]; }
    return v;
}

static uint64_t block_size(Rng& r) {
    const uint32_t kind = r.below(100);
    if (kind < 60) return 1 + r.below(256);
    if (kind < 85) return 256 + r.below(8192);
    if (kind < 99) return 8192 + r.below(256 * 1024);
    return 256 * 1024 + r.below(4 * 1024 * 1024);
}

struct Case { std::vector<uint64_t> sys, gfx; };

// miniz on its own, as rsc::inflate used it: the reference for what a stream gives (empty + threw for a corrupt one)
static Bytes miniz_inflate(const Bytes& data, size_t cap, bool& threw) {
    mz_stream z{};
    mz_inflateInit2(&z, -MZ_DEFAULT_WINDOW_BITS);
    Bytes out(cap);
    z.next_in = data.data(); z.avail_in = (unsigned)data.size();
    z.next_out = out.data(); z.avail_out = (unsigned)out.size();
    const int rc = mz_inflate(&z, MZ_FINISH);
    out.resize(z.total_out);
    mz_inflateEnd(&z);
    threw = rc != MZ_STREAM_END && rc != MZ_BUF_ERROR && rc != MZ_OK;
    return threw ? Bytes{} : out;
}

static Bytes deflate_with(const Bytes& in, int level, int strategy) {
    mz_stream z{};
    mz_deflateInit2(&z, level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, strategy);
    Bytes out(mz_deflateBound(&z, (mz_ulong)in.size()) + 64);
    z.next_in = in.data(); z.avail_in = (unsigned)in.size();
    z.next_out = out.data(); z.avail_out = (unsigned)out.size();
    mz_deflate(&z, MZ_FINISH);
    out.resize(z.total_out);
    mz_deflateEnd(&z);
    return out;
}

// rsc::inflate (the table-driven decoder, miniz for what it refuses) against miniz alone: streams of every block type
// (stored, fixed, dynamic) over data with every kind of match (runs, short and long distances, none), whole, with
// bytes flipped and cut short, into buffers that hold all of it or less.
static void inflate_checks(Rng& r) {
    int fast_bad = 0, same_bad = 0, total = 0, corrupt = 0;
    std::string first;
    for (int t = 0; t < 400; t++) {
        const size_t n = t < 10 ? (size_t)t : 1 + r.below(t % 7 == 0 ? 2000000 : 70000);
        Bytes in(n);
        const uint32_t kind = r.below(5);
        for (size_t i = 0; i < n; i++) {
            switch (kind) {
                case 0: in[i] = (uint8_t)r.next(); break;                                         // incompressible
                case 1: in[i] = (uint8_t)(r.below(8) ? (i ? in[i - 1] : 0) : r.next()); break;    // runs (distance 1)
                case 2: in[i] = (uint8_t)(i >= 3 && r.below(6) ? in[i - 3] : r.below(16)); break; // distance 3
                case 3: in[i] = (uint8_t)(i >= 9000 && r.below(3) ? in[i - 9000 + r.below(2)] : r.below(64)); break;
                default: in[i] = (uint8_t)("the quick brown fox "[i % 20] + (r.below(30) ? 0 : 1)); break;
            }
        }
        const int level = (int)r.below(11), strategy = r.below(4) == 0 ? MZ_FIXED : MZ_DEFAULT_STRATEGY;
        Bytes z = level == 9 && r.below(2) ? rsc::deflate(in) : deflate_with(in, level, strategy);
        // whole streams: the fast decoder takes them and gives the input back
        total++;
        size_t got = 0;
        Bytes buf(n + 1);
        if (!detail::inflate_fast(z.data(), z.size(), buf.data(), buf.size(), got) || got != n || !std::equal(in.begin(), in.end(), buf.begin())) {
            if (!fast_bad++) first = "fast decoder, case " + std::to_string(t) + " (" + std::to_string(n) + " bytes, level " + std::to_string(level) + ")";
        }
        // and the same streams damaged or cut, or into a buffer too small: rsc::inflate gives what miniz gives
        for (int v = 0; v < 4; v++) {
            Bytes zz = z;
            size_t cap = n;
            if (v == 1 && !zz.empty()) for (int k = 0; k < 3; k++) zz[r.below((uint32_t)zz.size())] ^= (uint8_t)(1 + r.below(255));
            if (v == 2) zz.resize(zz.size() * r.below(100) / 100);
            if (v == 3) cap = n / 2;
            bool threw_ref = false, threw = false;
            const Bytes ref = miniz_inflate(zz, cap, threw_ref);
            Bytes got2;
            try { got2 = rsc::inflate(zz, cap); } catch (const Error&) { threw = true; }
            corrupt += threw_ref;
            if (threw != threw_ref || (!threw && got2 != ref)) {
                if (!same_bad++ && first.empty()) first = "case " + std::to_string(t) + " variant " + std::to_string(v);
            }
        }
    }
    check(fast_bad == 0, "inflate: " + std::to_string(total) + " whole streams (stored, fixed, dynamic) decoded by the fast decoder" + (first.empty() ? "" : " (first bad: " + first + ")"));
    check(same_bad == 0, "inflate: " + std::to_string(total * 4) + " whole, damaged, cut and capped streams give what miniz gives (" + std::to_string(corrupt) + " corrupt)");
}

static bool run(const Case& cs, uint64_t seed, bool compress, double& fill, std::string& why) {
    Rng r{seed};
    Builder b;
    std::vector<BlockId> ids;
    std::vector<Bytes> want;
    for (size_t i = 0; i < cs.sys.size() + cs.gfx.size(); i++) {
        const bool sys = i < cs.sys.size();
        const uint64_t n = sys ? cs.sys[i] : cs.gfx[i - cs.sys.size()];
        const BlockId id = b.alloc(sys ? Segment::System : Segment::Graphics, (size_t)n);
        Bytes bytes((size_t)n);
        for (auto& x : bytes) x = (uint8_t)r.next();
        b.put(id, 0, bytes.data(), bytes.size());
        ids.push_back(id);
        want.push_back(std::move(bytes));
        if (i == 0) {
            const BlockId pi = b.alloc_pages_info();
            ids.push_back(pi);
            want.push_back({});
        }
    }
    struct Ptr { size_t from, at, to; uint64_t addend; };
    std::vector<Ptr> ptrs;
    for (size_t k = 0; k < ids.size() / 3; k++) {
        const size_t from = r.below((uint32_t)ids.size()), to = r.below((uint32_t)ids.size());
        if (from == 1 || want[from].size() < 24) continue;
        const size_t at = from == 0 ? 16 + 8 * r.below((uint32_t)(want[0].size() - 16) / 8) : 8 * r.below((uint32_t)want[from].size() / 8);
        bool clash = false;
        for (const Ptr& p : ptrs) clash |= p.from == from && p.at == at;
        if (clash) continue;
        ptrs.push_back({from, at, to, r.below(4) * 4u});
        b.pointer(ids[from], at, ids[to], ptrs.back().addend);
    }
    b.pointer(ids[0], 8, ids[1]);
    rsc::Resource res;
    try {
        const Bytes file = b.build(rsc::kYdr, compress);
        if (compress) res = rsc::Resource::load(file);
        else {
            std::memcpy(&res.system_flags.value, file.data() + 8, 4);
            std::memcpy(&res.graphics_flags.value, file.data() + 12, 4);
            const size_t sys = (size_t)res.system_flags.total_size();
            if (file.size() != 16 + sys + res.graphics_flags.total_size()) return (why = "the file is not its pages long", false);
            res.system.assign(file.begin() + 16, file.begin() + 16 + (ptrdiff_t)sys);
            res.graphics.assign(file.begin() + 16 + (ptrdiff_t)sys, file.end());
        }
    } catch (const std::exception& e) {
        why = e.what();
        return false;
    }
    auto fail = [&](const std::string& w) { why = w; return false; };
    const auto sp = pages_of(res.system_flags), gp = pages_of(res.graphics_flags);
    if (sp.size() + gp.size() > rsc::kMaxPages) return fail("more than 128 pages");
    for (const auto* ps : {&sp, &gp})
        for (const PageSpan& p : *ps)
            if (p.size < rsc::kMinPageSize) return fail("a page of " + std::to_string(p.size) + " bytes");
    if (b.position(ids[0]) != rsc::kSystemBase) return fail("the root is not at system offset 0");
    uint64_t used = 0;
    std::vector<std::vector<std::pair<uint64_t, uint64_t>>> spans(2);
    for (size_t i = 0; i < ids.size(); i++) {
        const bool sys = b.segment(ids[i]) == Segment::System;
        const uint64_t pos = b.position(ids[i]) - (sys ? rsc::kSystemBase : rsc::kGraphicsBase), n = b.size(ids[i]);
        used += (n + 15) & ~uint64_t(15);
        if (pos % 16) return fail("block " + std::to_string(i) + " is not 16-aligned");
        bool inside = false;
        for (const PageSpan& p : sys ? sp : gp) inside |= pos >= p.at && pos + n <= p.at + p.size;
        if (!inside) return fail("block " + std::to_string(i) + " of " + std::to_string(n) + " bytes at " + std::to_string(pos) + " crosses a page");
        spans[sys ? 0 : 1].push_back({pos, pos + std::max<uint64_t>(n, 1)});
        if (i == 1) continue;
        const Bytes& seg = sys ? res.system : res.graphics;
        Bytes got(seg.begin() + (ptrdiff_t)pos, seg.begin() + (ptrdiff_t)(pos + n));
        for (const Ptr& p : ptrs)
            if (p.from == i) std::memcpy(got.data() + p.at, want[i].data() + p.at, 8);
        if (i == 0) std::memcpy(got.data() + 8, want[0].data() + 8, 8);
        if (got != want[i]) return fail("block " + std::to_string(i) + " changed");
    }
    for (auto& v : spans) {
        std::sort(v.begin(), v.end());
        for (size_t k = 1; k < v.size(); k++)
            if (v[k].first < v[k - 1].second) return fail("two blocks overlap");
    }
    for (const Ptr& p : ptrs) {
        const bool sys = b.segment(ids[p.from]) == Segment::System;
        uint64_t v = 0;
        std::memcpy(&v, (sys ? res.system : res.graphics).data() + (b.position(ids[p.from]) - (sys ? rsc::kSystemBase : rsc::kGraphicsBase)) + p.at, 8);
        if (v != b.position(ids[p.to]) + p.addend) return fail("a pointer is not patched");
    }
    uint64_t pi = 0;
    std::memcpy(&pi, res.system.data() + 8, 8);
    if (pi != b.position(ids[1])) return fail("the root does not point at the page map");
    const uint8_t* m = res.system.data() + (pi - rsc::kSystemBase);
    if (m[8] != sp.size() || m[9] != gp.size()) return fail("the page map's counts are wrong");
    if (b.size(ids[1]) < 16 + 8 * (sp.size() + gp.size())) return fail("the page map is too small for its pages");
    if (b.size(ids[1]) > 16 + 8 * (sp.size() + gp.size() + 2)) return fail("the page map is larger than its pages need");
    fill = (double)used / (double)(res.system.size() + res.graphics.size());
    return true;
}

int main() {
    {
        Rng r{7};
        bool ok = true;
        for (int t = 0; t < 2000; t++) {
            std::array<uint32_t, 9> c{};
            for (int i = 0; i < 9; i++) c[(size_t)i] = r.below(rsc::PageFlags::limit(i) + 1);
            const uint32_t shift = r.below(16);
            const rsc::PageFlags f = rsc::PageFlags::make(shift, c);
            uint64_t total = 0;
            for (int i = 0; i < 9; i++) total += (uint64_t)c[(size_t)i] * ((0x200ull << shift) << (8 - i));
            ok &= f.counts() == c && f.base_shift() == shift && f.total_size() == total;
        }
        check(ok, "page flags: 2000 random class counts and shifts encode and decode");
        bool threw = false;
        try { rsc::PageFlags::make(0, {2, 0, 0, 0, 0, 0, 0, 0, 0}); } catch (const Error&) { threw = true; }
        check(threw, "page flags: two pages of the largest class are refused");
    }

    std::vector<std::pair<std::string, Case>> fixed;
    fixed.push_back({"a lone root", {{64}, {}}});
    fixed.push_back({"a root of exactly 8 KB", {{0x2000}, {}}});
    fixed.push_back({"a root just over 8 KB", {{0x2001}, {}}});
    fixed.push_back({"a 3 MB graphics block", {{256}, {3 << 20}}});
    fixed.push_back({"a 40 MB graphics block", {{256}, {40u << 20}}});
    {
        Case c{{128}, {}};
        for (int i = 0; i < 5000; i++) c.sys.push_back(48);
        fixed.push_back({"5000 small system blocks", c});
    }
    {
        Case c{{128}, {}};
        for (int i = 0; i < 300; i++) c.gfx.push_back(0x20000);
        fixed.push_back({"300 graphics blocks of 128 KB (too many pages at the smallest shift)", c});
    }
    {
        Case c{{128}, {}};
        for (int i = 0; i < 200; i++) c.gfx.push_back(0x10001);
        fixed.push_back({"200 graphics blocks just over 64 KB", c});
    }
    for (auto& [name, cs] : fixed) {
        double fill = 0;
        std::string why;
        const bool ok = run(cs, 1, false, fill, why);
        char buf[64];
        std::snprintf(buf, sizeof buf, " (blocks fill %.1f%%)", fill * 100);
        check(ok, name + (ok ? buf : ": " + why));
    }

    Rng r{12345};
    int bad = 0;
    double fill_sum = 0, fill_min = 1;
    std::string first;
    const int cases = 600;
    for (int t = 0; t < cases; t++) {
        Case cs;
        const uint32_t ns = 1 + r.below(r.below(4) ? 60 : 600), ng = r.below(3) ? r.below(r.below(4) ? 30 : 200) : 0;
        cs.sys.push_back(64 + r.below(256));
        for (uint32_t i = 1; i < ns; i++) cs.sys.push_back(block_size(r));
        for (uint32_t i = 0; i < ng; i++) cs.gfx.push_back(block_size(r));
        double fill = 0;
        std::string why;
        if (!run(cs, (uint64_t)t * 977 + 3, t % 50 == 0, fill, why)) {
            if (!bad++) first = "case " + std::to_string(t) + ": " + why;
            continue;
        }
        fill_sum += fill;
        fill_min = std::min(fill_min, fill);
    }
    check(bad == 0, std::to_string(cases) + " random block sets lay out validly" + (bad ? " (" + std::to_string(bad) + " bad, first " + first + ")" : ""));
    std::printf("blocks fill %.1f%% of the pages on average, %.1f%% at worst\n", 100 * fill_sum / cases, 100 * fill_min);

    inflate_checks(r);
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
