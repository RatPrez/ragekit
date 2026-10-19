// Reading and writing graphs of RscBlocks (graph.h): what a format's schema code uses.
#pragma once
#include <functional>
#include <map>
#include <string>
#include "ragekit/graph.h"
#include "ragekit/rsc.h"

namespace rk {

class GraphReader {
public:
    explicit GraphReader(const Reader& r, const char* format) : r_(r), format_(format) {}
    const Reader& reader() const { return r_; }

    using Fill = std::function<void(RscBlock&, uint64_t addr)>;
    /// The block of `size` bytes at `addr`, read once per address: a second request for the same address returns the first
    /// block (of the same kind and size, else it throws). `fill` takes the new block's pointers.
    RscBlockPtr block(uint64_t addr, uint16_t kind, size_t size, const Fill& fill = {});
    /// Takes the pointer at `at` out of the block (its bytes become 0) and returns it.
    uint64_t take(RscBlock& b, uint32_t at);
    /// Takes the pointer at `at` and links it to what `make` reads there (nothing for a null pointer).
    void own(RscBlock& b, uint32_t at, const std::function<RscBlockPtr(uint64_t)>& make);
    /// A pointer at `at` to a NUL terminated string.
    void string(RscBlock& b, uint32_t at, uint16_t kind);
    /// Takes the pointer at `at`, to be linked by resolve() to whichever block holds its target.
    void weak(RscBlock& b, uint32_t at);
    /// Marks the 8 bytes at `at` as data the census must not take for a pointer (a name hash followed by 0).
    void data(RscBlock& b, uint32_t at) { data_[&b].push_back(at); }
    /// Links the weak pointers (after every block is read) and sorts every block's links.
    void resolve();
    /// 8-byte values inside the blocks that look like pointers into the system segment where no link was taken: the schema's
    /// blind spots (0 on a complete schema). `first` gets a description of the first few. Blocks of the `raw` kinds (plain
    /// data such as compressed animation) are not looked at.
    size_t census(std::string* first = nullptr, std::initializer_list<uint16_t> raw = {}) const;

private:
    struct Weak { RscBlock* from; uint32_t at; uint64_t target; };
    const Reader& r_;
    const char* format_;
    std::map<uint64_t, RscBlockPtr> at_;
    std::vector<Weak> weak_;
    std::map<const RscBlock*, std::vector<uint32_t>> data_;
};

/// Lays out every block reachable from `root` (the root first, so at the start of the system segment) and writes their
/// bytes and pointers; the pointer at root + `page_map_at` is set to the resource's page map.
void write_graph(Builder& b, const RscBlockPtr& root, size_t page_map_at = 8);

}  // namespace rk
