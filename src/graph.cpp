#include "ragekit/graph.h"
#include <algorithm>
#include <cstdio>
#include <set>
#include <unordered_map>
#include "ragekit/error.h"
#include "graph_io.h"

namespace rk {

std::string RscBlock::string_at(uint32_t at) const {
    const Link* l = link(at);
    if (!l || !l->to) return "";
    const Bytes& b = l->to->bytes;
    const size_t start = std::min<size_t>(l->offset, b.size());
    size_t end = start;
    while (end < b.size() && b[end]) end++;
    return std::string((const char*)b.data() + start, end - start);
}

void RscBlock::set_link(uint32_t at, std::shared_ptr<RscBlock> to, uint32_t offset) {
    auto it = std::find_if(links.begin(), links.end(), [&](const Link& l) { return l.at == at; });
    if (!to) {
        if (it != links.end()) links.erase(it);
        return;
    }
    if (it != links.end()) { it->to = std::move(to); it->offset = offset; return; }
    links.push_back({at, std::move(to), offset});
    std::sort(links.begin(), links.end(), [](const Link& x, const Link& y) { return x.at < y.at; });
}

std::vector<RscBlockPtr> graph_blocks(const RscBlockPtr& root) {
    std::vector<RscBlockPtr> out;
    if (!root) return out;
    std::set<const RscBlock*> seen;
    std::vector<RscBlockPtr> stack{root};
    while (!stack.empty()) {
        RscBlockPtr b = stack.back();
        stack.pop_back();
        if (!b || !seen.insert(b.get()).second) continue;
        out.push_back(b);
        for (auto it = b->links.rbegin(); it != b->links.rend(); ++it) if (it->to && !seen.count(it->to.get())) stack.push_back(it->to);
    }
    return out;
}

bool same_graph(const RscBlockPtr& a, const RscBlockPtr& b, std::string* where) {
    auto fail = [&](const std::string& w) { if (where) *where = w; return false; };
    if (!a || !b) return (bool)a == (bool)b ? true : fail("one graph is empty");
    std::unordered_map<const RscBlock*, const RscBlock*> ab, ba;
    std::vector<std::pair<const RscBlock*, const RscBlock*>> stack{{a.get(), b.get()}};
    size_t n = 0;
    while (!stack.empty()) {
        auto [x, y] = stack.back();
        stack.pop_back();
        auto i = ab.find(x);
        auto j = ba.find(y);
        if (i != ab.end() || j != ba.end()) {
            if (i == ab.end() || j == ba.end() || i->second != y || j->second != x) return fail("a block shared in one graph is not shared the same way in the other");
            continue;
        }
        ab[x] = y;
        ba[y] = x;
        const std::string id = "block " + std::to_string(n++) + " (kind " + std::to_string(x->kind) + ")";
        if (x->kind != y->kind) return fail(id + ": kind " + std::to_string(y->kind) + " in the other");
        if (x->bytes != y->bytes) {
            size_t k = 0;
            while (k < x->bytes.size() && k < y->bytes.size() && x->bytes[k] == y->bytes[k]) k++;
            return fail(id + ": bytes differ at " + std::to_string(k) + " (" + std::to_string(x->bytes.size()) + " / " + std::to_string(y->bytes.size()) + " bytes)");
        }
        if (x->links.size() != y->links.size()) return fail(id + ": " + std::to_string(x->links.size()) + " links, " + std::to_string(y->links.size()) + " in the other");
        for (size_t k = 0; k < x->links.size(); k++) {
            const auto &p = x->links[k], &q = y->links[k];
            if (p.at != q.at || p.offset != q.offset || (bool)p.to != (bool)q.to) return fail(id + ": link " + std::to_string(k) + " at " + std::to_string(p.at) + " differs");
            if (p.to) stack.push_back({p.to.get(), q.to.get()});
        }
    }
    return true;
}

// ---------------------------------------------------------------- reading

RscBlockPtr GraphReader::block(uint64_t addr, uint16_t kind, size_t size, const Fill& fill) {
    if (!addr) return nullptr;
    auto it = at_.find(addr);
    if (it != at_.end()) {
        if (it->second->kind != kind || it->second->bytes.size() != size)
            throw Error(std::string(format_) + ": the structure at " + rsc::hex_ptr(addr) + " is read as kind " + std::to_string(kind) + " of " + std::to_string(size) +
                        " bytes and as kind " + std::to_string(it->second->kind) + " of " + std::to_string(it->second->bytes.size()));
        return it->second;
    }
    if (!r_.resource().in_system(addr)) throw Error(std::string(format_) + ": a structure at " + rsc::hex_ptr(addr) + " outside the system segment");
    auto b = std::make_shared<RscBlock>();
    b->kind = kind;
    b->bytes = r_.bytes(addr, size);
    at_[addr] = b;
    if (fill) fill(*b, addr);
    return b;
}

uint64_t GraphReader::take(RscBlock& b, uint32_t at) {
    if (at + 8 > b.bytes.size()) throw Error(std::string(format_) + ": a pointer field past its structure");
    uint64_t v;
    std::memcpy(&v, b.bytes.data() + at, 8);
    std::memset(b.bytes.data() + at, 0, 8);
    return v;
}

void GraphReader::own(RscBlock& b, uint32_t at, const std::function<RscBlockPtr(uint64_t)>& make) {
    const uint64_t p = take(b, at);
    if (!p) return;
    RscBlockPtr to = make(p);
    if (to) b.links.push_back({at, std::move(to), 0});
}

void GraphReader::string(RscBlock& b, uint32_t at, uint16_t kind) {
    own(b, at, [&](uint64_t p) {
        if (auto it = at_.find(p); it != at_.end()) return it->second;   // a string two structures share
        const std::string s = r_.str(p);
        return block(p, kind, s.size() + 1);
    });
}

void GraphReader::weak(RscBlock& b, uint32_t at) {
    const uint64_t p = take(b, at);
    if (p) weak_.push_back({&b, at, p});
}

void GraphReader::resolve() {
    for (const Weak& w : weak_) {
        auto it = at_.upper_bound(w.target);
        if (it == at_.begin()) throw Error(std::string(format_) + ": a pointer (" + rsc::hex_ptr(w.target) + ") leads before every structure");
        --it;
        if (w.target >= it->first + std::max<size_t>(it->second->bytes.size(), 1))
            throw Error(std::string(format_) + ": a pointer (" + rsc::hex_ptr(w.target) + ") leads to no structure read");
        w.from->links.push_back({w.at, it->second, (uint32_t)(w.target - it->first)});
    }
    weak_.clear();
    for (auto& [addr, b] : at_) std::sort(b->links.begin(), b->links.end(), [](const RscBlock::Link& x, const RscBlock::Link& y) { return x.at < y.at; });
}

size_t GraphReader::census(std::string* first, std::initializer_list<uint16_t> raw) const {
    size_t n = 0;
    const uint64_t size = r_.resource().system.size();
    for (auto& [addr, b] : at_) {
        if (std::find(raw.begin(), raw.end(), b->kind) != raw.end()) continue;
        std::set<uint32_t> known;
        for (const RscBlock::Link& l : b->links) known.insert(l.at);
        if (auto d = data_.find(b.get()); d != data_.end()) known.insert(d->second.begin(), d->second.end());
        for (uint32_t o = (uint32_t)((8 - (addr & 7)) & 7); o + 8 <= b->bytes.size(); o += 8) {
            if (known.count(o)) continue;
            uint64_t v;
            std::memcpy(&v, b->bytes.data() + o, 8);
            if ((v & 0xFFFFFFFFF0000000ull) == rsc::kSystemBase && v - rsc::kSystemBase < size && v != rsc::kSystemBase) {
                if (first && n < 4) {
                    char s[96];
                    std::snprintf(s, sizeof s, " kind %u +0x%X = %s;", b->kind, o, rsc::hex_ptr(v).c_str());
                    *first += s;
                }
                n++;
            }
        }
    }
    return n;
}

// ---------------------------------------------------------------- writing

void write_graph(Builder& b, const RscBlockPtr& root, size_t page_map_at) {
    if (!root) throw Error("write_graph: no root");
    const std::vector<RscBlockPtr> blocks = graph_blocks(root);
    std::unordered_map<const RscBlock*, BlockId> ids;
    for (const RscBlockPtr& blk : blocks) {
        BlockId id = b.alloc(Segment::System, std::max<size_t>(blk->bytes.size(), 1));
        if (!blk->bytes.empty()) b.put(id, 0, blk->bytes.data(), blk->bytes.size());
        ids[blk.get()] = id;
        if (blk == root) b.pointer(id, page_map_at, b.alloc_pages_info());
    }
    for (const RscBlockPtr& blk : blocks)
        for (const RscBlock::Link& l : blk->links) {
            if (!l.to) continue;
            if (l.at + 8 > blk->bytes.size()) throw Error("write_graph: a link past its structure");
            if (l.offset > l.to->bytes.size()) throw Error("write_graph: a link past the end of its target");
            b.pointer(ids.at(blk.get()), l.at, ids.at(l.to.get()), l.offset);
        }
}

}  // namespace rk
