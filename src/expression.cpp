#include "ragekit/expression.h"
#include "ragekit/error.h"
#include "graph_io.h"

// pgDictionary (0x40): 0x08 page map, 0x20 name hashes (ptr, count:u16, capacity:u16), 0x30 items (pointer list: ptr,
// count:u16, capacity:u16). CodeWalker's Expression.cs and Filter.cs name the rest:
//   crExpressions 0x90     0x20 streams (pointer list), 0x30 tracks (4 bytes each), 0x40 springs (0xA0 bytes each),
//                          0x50 variables (name hashes), 0x60 name (ptr, length:u16, capacity:u16), 0x74 signature,
//                          0x78 the longest stream
//   expression stream      0x00 name hash, 0x04 / 0x08 / 0x0C (u16) the lengths of its three data blocks, 0x0E depth; the data
//   crFrameFilter 0x18     0x0C signature, 0x10 type; multi-weight (type 4, 0x40 bytes): 0x18 entries (8 bytes: 0, track,
//                          bone, weight index), 0x28 weights (floats)

namespace rk {

namespace {

template <class E> constexpr uint16_t K(E k) { return (uint16_t)k; }

/// The pgDictionary root, its two lists, and an item reader.
RscBlockPtr dictionary(GraphReader& g, const Reader& r, uint16_t kind, const std::function<RscBlockPtr(uint64_t)>& item) {
    return g.block(rsc::kSystemBase, kind, 0x40, [&](RscBlock& b, uint64_t a) {
        g.take(b, 0x08);
        const size_t names = std::max(r.u16(a + 0x28), r.u16(a + 0x2A));
        g.own(b, 0x20, [&](uint64_t p) { return names ? g.block(p, K(ExpressionKind::Hashes), names * 4) : nullptr; });
        const size_t n = std::max(r.u16(a + 0x38), r.u16(a + 0x3A));
        g.own(b, 0x30, [&](uint64_t p) {
            return n ? g.block(p, K(ExpressionKind::PointerArray), n * 8, [&](RscBlock& arr, uint64_t) {
                for (size_t i = 0; i < n; i++) g.own(arr, (uint32_t)(i * 8), item);
            }) : nullptr;
        });
    });
}

/// A (ptr, count:u16, capacity:u16) list of `size`-byte elements at `at` of `b` as a raw block.
void raw_list(GraphReader& g, const Reader& r, RscBlock& b, uint64_t a, uint32_t at, uint16_t kind, size_t size) {
    const size_t n = std::max(r.u16(a + at + 8), r.u16(a + at + 10));
    g.own(b, at, [&](uint64_t p) { return n ? g.block(p, kind, n * size) : nullptr; });
}

std::vector<std::pair<uint32_t, RscBlockPtr>> items(const RscBlockPtr& root) {
    std::vector<std::pair<uint32_t, RscBlockPtr>> out;
    if (!root) return out;
    RscBlockPtr names = root->target(0x20), arr = root->target(0x30);
    const size_t n = root->get<uint16_t>(0x38);
    for (size_t i = 0; i < n; i++) {
        const uint32_t h = names && (i + 1) * 4 <= names->bytes.size() ? names->get<uint32_t>(i * 4) : 0;
        out.push_back({h, arr ? arr->target((uint32_t)(i * 8)) : nullptr});
    }
    return out;
}

Bytes write(const RscBlockPtr& root, int version) {
    Builder b;
    write_graph(b, root);
    return b.build(version);
}

}  // namespace

std::vector<ExpressionTrack> ExpressionRef::tracks() const {
    std::vector<ExpressionTrack> v;
    RscBlockPtr t = block ? block->target(0x30) : nullptr;
    if (!t) return v;
    v.resize(std::min<size_t>(block->get<uint16_t>(0x38), t->bytes.size() / 4));
    if (!v.empty()) std::memcpy(v.data(), t->bytes.data(), v.size() * 4);
    return v;
}

std::vector<uint32_t> ExpressionRef::variables() const {
    std::vector<uint32_t> v;
    RscBlockPtr t = block ? block->target(0x50) : nullptr;
    if (!t) return v;
    v.resize(std::min<size_t>(block->get<uint16_t>(0x58), t->bytes.size() / 4));
    if (!v.empty()) std::memcpy(v.data(), t->bytes.data(), v.size() * 4);
    return v;
}

std::vector<RscBlockPtr> ExpressionRef::streams() const {
    std::vector<RscBlockPtr> v;
    RscBlockPtr arr = block ? block->target(0x20) : nullptr;
    if (!arr) return v;
    for (const RscBlock::Link& l : arr->links) if (l.at / 8 < stream_count()) v.push_back(l.to);
    return v;
}

std::vector<ExpressionRef> ExpressionDictionary::expressions() const {
    std::vector<ExpressionRef> v;
    for (auto& [h, b] : items(root)) if (b) v.push_back({h, b});
    return v;
}

std::vector<float> FrameFilterRef::weights() const {
    std::vector<float> v;
    RscBlockPtr w = type() == 4 ? block->target(0x28) : nullptr;
    if (!w) return v;
    v.resize(std::min<size_t>(block->get<uint16_t>(0x30), w->bytes.size() / 4));
    if (!v.empty()) std::memcpy(v.data(), w->bytes.data(), v.size() * 4);
    return v;
}

std::vector<FrameFilterRef> FrameFilterDictionary::filters() const {
    std::vector<FrameFilterRef> v;
    for (auto& [h, b] : items(root)) if (b) v.push_back({h, b});
    return v;
}

ExpressionDictionary read_yed(std::span<const uint8_t> file, size_t* census, std::string* where) {
    const rsc::Resource res = rsc::Resource::load(file);
    if (res.version != 25) throw Error("yed: resource version " + std::to_string(res.version) + ", expected 25");
    const Reader r(res);
    GraphReader g(r, "yed");
    ExpressionDictionary d;
    d.root = dictionary(g, r, K(ExpressionKind::Dictionary), [&](uint64_t p) {
        return g.block(p, K(ExpressionKind::Expression), 0x90, [&](RscBlock& e, uint64_t a) {
            const size_t n = std::max(r.u16(a + 0x28), r.u16(a + 0x2A));
            g.own(e, 0x20, [&](uint64_t q) {
                return n ? g.block(q, K(ExpressionKind::PointerArray), n * 8, [&](RscBlock& arr, uint64_t) {
                    for (size_t i = 0; i < n; i++)
                        g.own(arr, (uint32_t)(i * 8), [&](uint64_t s) {
                            const size_t len = 16 + (size_t)r.u32(s + 4) + r.u32(s + 8) + r.u16(s + 12);
                            return g.block(s, K(ExpressionKind::Stream), len);
                        });
                }) : nullptr;
            });
            raw_list(g, r, e, a, 0x30, K(ExpressionKind::Tracks), 4);
            raw_list(g, r, e, a, 0x40, K(ExpressionKind::Springs), 0xA0);
            raw_list(g, r, e, a, 0x50, K(ExpressionKind::Hashes), 4);
            g.string(e, 0x60, K(ExpressionKind::String));
        });
    });
    g.resolve();
    if (census) *census = g.census(where, {K(ExpressionKind::Stream), K(ExpressionKind::Springs), K(ExpressionKind::Hashes), K(ExpressionKind::Tracks),
                                           K(ExpressionKind::String)});
    return d;
}

Bytes write_yed(const ExpressionDictionary& d) { return write(d.root, 25); }

FrameFilterDictionary read_yfd(std::span<const uint8_t> file, size_t* census, std::string* where) {
    const rsc::Resource res = rsc::Resource::load(file);
    if (res.version != 4) throw Error("yfd: resource version " + std::to_string(res.version) + ", expected 4");
    const Reader r(res);
    GraphReader g(r, "yfd");
    FrameFilterDictionary d;
    d.root = dictionary(g, r, K(ExpressionKind::FilterDictionary), [&](uint64_t p) {
        const uint32_t type = r.u32(p + 0x10);
        if (type != 4) throw Error("yfd: frame filter type " + std::to_string(type) + " is not known (4, multi-weight, is)");
        return g.block(p, K(ExpressionKind::Filter), 0x40, [&](RscBlock& f, uint64_t a) {
            raw_list(g, r, f, a, 0x18, K(ExpressionKind::FilterEntries), 8);
            raw_list(g, r, f, a, 0x28, K(ExpressionKind::FilterWeights), 4);
        });
    });
    g.resolve();
    if (census) *census = g.census(where, {K(ExpressionKind::FilterEntries), K(ExpressionKind::FilterWeights), K(ExpressionKind::Hashes)});
    return d;
}

Bytes write_yfd(const FrameFilterDictionary& d) { return write(d.root, 4); }

}  // namespace rk
