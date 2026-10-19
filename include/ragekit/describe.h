// ragekit - what a file holds, as a tree of facts a tool can show: the summary `rk info` prints (rk info prints this
// tree), for every kind the library reads.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "types.h"

namespace rk {

struct Fact {
    std::string label;          // "textures", "lod high", "shader 3" ...
    std::string value;          // "12", "normal_spec (bucket 0)" ... ("" for a heading)
    std::vector<Fact> children;

    /// Adds a child and returns it.
    Fact& add(std::string label, std::string value = {});
};

struct DescribeOptions {
    bool verbose = false;       // every item (shader parameters, bones, all strings ...), not only the first ones
    size_t max_items = 20;      // without verbose: lists stop after this many (a last child says how many more)
};

/// The facts of a file, chosen by its content and the extension of `name` (a path or a file name). Throws what the
/// format's reader throws; a kind the library has no reader for gives a root that says so.
Fact describe(std::span<const uint8_t> file, const std::string& name, const DescribeOptions& opt = {});
/// The tree as text: a line per fact, "label: value", indented two spaces per level (what rk info prints).
std::string format_facts(const Fact& root);

}  // namespace rk
