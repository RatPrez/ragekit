#include "ragekit/script.h"
#include "ragekit/error.h"
#include "graph_io.h"

// The header (0x80 bytes): 0x00 the class's vtable (a run-time address), 0x08 page map, 0x10 code pages (pointer array),
// 0x18 globals signature, 0x1C code size, 0x20 parameters, 0x24 statics, 0x28 globals (block << 18 | count), 0x2C
// natives, 0x30 statics (8 bytes each), 0x38 globals pages, 0x40 natives (8 bytes each), 0x58 name hash, 0x5C 1,
// 0x60 name, 0x68 string pages, 0x70 string size. Pages hold 0x4000 bytes, the last one the rest.

namespace rk {

namespace {

constexpr uint32_t kPage = 0x4000;          // code and string pages
constexpr uint32_t kGlobalsPage = 0x20000;  // globals pages: 16,384 globals each
constexpr uint16_t K(ScriptKind k) { return (uint16_t)k; }

Bytes join_pages(const RscBlockPtr& arr) {
    Bytes out;
    if (!arr) return out;
    for (const RscBlock::Link& l : arr->links) if (l.to) out.insert(out.end(), l.to->bytes.begin(), l.to->bytes.end());
    return out;
}

template <class T> std::vector<T> items(const RscBlockPtr& b, size_t n) {
    std::vector<T> v;
    if (!b) return v;
    v.resize(std::min(n, b->bytes.size() / sizeof(T)));
    if (!v.empty()) std::memcpy(v.data(), b->bytes.data(), v.size() * sizeof(T));
    return v;
}

}  // namespace

Bytes Script::code() const { Bytes b = join_pages(root->target(0x10)); b.resize(std::min<size_t>(b.size(), code_size())); return b; }
Bytes Script::strings() const { Bytes b = join_pages(root->target(0x68)); b.resize(std::min<size_t>(b.size(), string_size())); return b; }
std::vector<uint64_t> Script::statics() const { return items<uint64_t>(root->target(0x30), static_count()); }

std::vector<uint64_t> Script::natives() const {
    std::vector<uint64_t> v = items<uint64_t>(root->target(0x40), native_count());
    for (size_t i = 0; i < v.size(); i++) {
        const unsigned r = (unsigned)((code_size() + i) & 63);
        v[i] = r ? (v[i] << r | v[i] >> (64 - r)) : v[i];
    }
    return v;
}

Script read_ysc(std::span<const uint8_t> file, size_t* census, std::string* where) {
    const rsc::Resource res = rsc::Resource::load(file);
    if (res.version != 12 && res.version != 10) throw Error("ysc: resource version " + std::to_string(res.version) + ", expected 10 or 12");
    const Reader r(res);
    GraphReader g(r, "ysc");
    auto pages = [&](RscBlock& b, uint32_t at, uint64_t size, uint16_t kind, uint64_t page = kPage) {
        const size_t n = (size_t)((size + page - 1) / page);
        g.own(b, at, [&](uint64_t p) {
            return n ? g.block(p, K(ScriptKind::PageArray), n * 8, [&](RscBlock& arr, uint64_t) {
                for (size_t i = 0; i < n; i++)
                    g.own(arr, (uint32_t)(i * 8), [&](uint64_t q) { return g.block(q, kind, (size_t)std::min<uint64_t>(page, size - i * page)); });
            }) : nullptr;
        });
    };
    Script s;
    s.version = res.version;
    s.root = g.block(rsc::kSystemBase, K(ScriptKind::Header), 0x80, [&](RscBlock& b, uint64_t a) {
        g.take(b, 0x08);
        pages(b, 0x10, r.u32(a + 0x1C), K(ScriptKind::CodePage));
        const uint32_t statics = r.u32(a + 0x24), globals = r.u32(a + 0x28) & 0x3FFFF, natives = r.u32(a + 0x2C);
        g.own(b, 0x30, [&](uint64_t p) { return statics ? g.block(p, K(ScriptKind::Statics), (size_t)statics * 8) : nullptr; });
        pages(b, 0x38, (uint64_t)globals * 8, K(ScriptKind::Globals), kGlobalsPage);
        g.own(b, 0x40, [&](uint64_t p) { return natives ? g.block(p, K(ScriptKind::Natives), (size_t)natives * 8) : nullptr; });
        g.string(b, 0x60, K(ScriptKind::String));
        pages(b, 0x68, r.u32(a + 0x70), K(ScriptKind::StringPage));
    });
    g.resolve();
    if (census) *census = g.census(where, {K(ScriptKind::CodePage), K(ScriptKind::StringPage), K(ScriptKind::Statics), K(ScriptKind::Globals),
                                           K(ScriptKind::Natives), K(ScriptKind::String)});
    return s;
}

Bytes write_ysc(const Script& s) {
    Builder b;
    write_graph(b, s.root);
    return b.build(s.version);
}

}  // namespace rk
