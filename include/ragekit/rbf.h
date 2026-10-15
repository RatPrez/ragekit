// ragekit - RBF (magic "RBF0"), the binary XML of creature metadata (.ymt: the expressions of ped components and props),
// cutscene and Bink metadata, some gtxd.ymt and _manifest.ymf files.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "types.h"

namespace rk {

enum class RbfType : uint8_t { U32 = 0x10, True = 0x20, False = 0x30, Float = 0x40, Float3 = 0x50, String = 0x60 };

struct RbfValue {
    RbfType type = RbfType::U32;
    uint32_t u32 = 0;
    float f[3] = {};   // Float: f[0]
    std::string str;

    static RbfValue of(uint32_t v) { RbfValue r; r.u32 = v; return r; }
    static RbfValue of(float v) { RbfValue r; r.type = RbfType::Float; r.f[0] = v; return r; }
    static RbfValue of(bool v) { RbfValue r; r.type = v ? RbfType::True : RbfType::False; return r; }
    static RbfValue of(std::string v) { RbfValue r; r.type = RbfType::String; r.str = std::move(v); return r; }
    bool boolean() const { return type == RbfType::True; }
    float number() const { return type == RbfType::Float ? f[0] : (float)u32; }
};

// An element (attributes, children), a value child (<name value="..."/>), or raw content (a text or array payload).
struct RbfNode {
    enum class Kind : uint8_t { Element, Value, Content };
    Kind kind = Kind::Element;
    std::string name;
    RbfValue value;
    Bytes content;
    std::vector<std::pair<std::string, RbfValue>> attributes;
    std::vector<RbfNode> children;

    const RbfNode* child(std::string_view name) const;
    std::vector<const RbfNode*> children_named(std::string_view name) const;
    const RbfValue* attribute(std::string_view name) const;
    /// The text of the first content child ("" without one), its terminating NUL dropped.
    std::string text() const;
};

bool is_rbf(std::span<const uint8_t> file);
RbfNode read_rbf(std::span<const uint8_t> file);
Bytes write_rbf(const RbfNode& root);
std::string rbf_dump(const RbfNode& root);

}  // namespace rk
