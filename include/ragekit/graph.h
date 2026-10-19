// ragekit - resource structures kept as read: a block per structure (or array of structures), its bytes with the
// pointers taken out and links in their place. Formats whose structures ragekit does not model field by field (the
// clips and animations of .ycd files, expressions, frame filters) read into a graph of these and write it back as it
// was; typed accessors read and change the fields tools need.
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "types.h"

namespace rk {

struct RscBlock {
    uint16_t kind = 0;      // the structure, in the format's own numbering
    Bytes bytes;            // the structure as stored, its pointer fields 0
    struct Link {
        uint32_t at = 0;                  // the pointer's offset in `bytes`
        std::shared_ptr<RscBlock> to;
        uint32_t offset = 0;              // into `to` (a pointer into the middle of a structure)
    };
    std::vector<Link> links;              // sorted by `at`

    template <class T> T get(size_t at) const { T v{}; if (at + sizeof(T) <= bytes.size()) std::memcpy(&v, bytes.data() + at, sizeof(T)); return v; }
    template <class T> void set(size_t at, T v) { if (at + sizeof(T) <= bytes.size()) std::memcpy(bytes.data() + at, &v, sizeof(T)); }
    const Link* link(uint32_t at) const { for (const Link& l : links) if (l.at == at) return &l; return nullptr; }
    /// The block a pointer at `at` leads to, nullptr for none.
    std::shared_ptr<RscBlock> target(uint32_t at) const { const Link* l = link(at); return l ? l->to : nullptr; }
    /// The NUL terminated string a pointer at `at` leads to, "" for none.
    std::string string_at(uint32_t at) const;
    /// Sets (or adds, or with no target removes) the pointer at `at`.
    void set_link(uint32_t at, std::shared_ptr<RscBlock> to, uint32_t offset = 0);
};
using RscBlockPtr = std::shared_ptr<RscBlock>;

/// Two graphs hold the same: the same kinds and bytes, links at the same places to blocks that are the same in turn,
/// and blocks shared in one shared the same way in the other. On a difference, `where` says the first one.
bool same_graph(const RscBlockPtr& a, const RscBlockPtr& b, std::string* where = nullptr);
/// Every block reachable from `root`, each once, in the order the writers lay them out (depth first, links in order).
std::vector<RscBlockPtr> graph_blocks(const RscBlockPtr& root);

}  // namespace rk
