#include "ragekit/rbf.h"
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include "ragekit/error.h"

// RBF0, then one token per item: a name index (u8) and a type (u8).
//   FF FF                 closes the open element
//   FD FF len:u32 bytes   raw content of the open element
//   idx type ...          a named item; an index one past the last declared one declares the name: len:u16 chars
//     00                  element: u32 (0), attribute count:u16, that many named values, then children up to FF FF
//     10 u32 | 20 true | 30 false | 40 float | 50 float x3 | 60 len:u16 chars
// A name is declared once, at its first use in document order, whatever its type.

namespace rk {

namespace {

constexpr uint8_t kClose = 0xFF, kContent = 0xFD;
constexpr size_t kMaxDepth = 512;

struct In {
    std::span<const uint8_t> b;
    size_t p = 4;
    std::vector<std::string> names;

    void need(size_t n) const {
        if (p + n > b.size()) throw Error("RBF: the file ends inside an item at byte " + std::to_string(p));
    }
    template <class T> T get() {
        need(sizeof(T));
        T v;
        std::memcpy(&v, b.data() + p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    std::string chars(size_t n) {
        need(n);
        std::string s((const char*)b.data() + p, n);
        p += n;
        return s;
    }
    std::string name(uint8_t idx) {
        if (idx == names.size()) names.push_back(chars(get<uint16_t>()));
        else if (idx > names.size()) throw Error("RBF: name " + std::to_string(idx) + " used before it is declared, at byte " + std::to_string(p));
        return names[idx];
    }
    RbfValue value(uint8_t type) {
        RbfValue v;
        v.type = (RbfType)type;
        switch (type) {
            case 0x10: v.u32 = get<uint32_t>(); break;
            case 0x20: case 0x30: break;
            case 0x40: v.f[0] = get<float>(); break;
            case 0x50: for (float& x : v.f) x = get<float>(); break;
            case 0x60: v.str = chars(get<uint16_t>()); break;
            default: throw Error("RBF: unknown value type " + std::to_string(type) + " at byte " + std::to_string(p - 2));
        }
        return v;
    }
};

struct Out {
    Bytes b;
    std::unordered_map<std::string, uint8_t> ids;

    void raw(const void* src, size_t n) { b.insert(b.end(), (const uint8_t*)src, (const uint8_t*)src + n); }
    template <class T> void put(T v) { raw(&v, sizeof(T)); }
    void token(const std::string& name, uint8_t type) {
        auto it = ids.find(name);
        if (it != ids.end()) {
            put(it->second);
            put(type);
            return;
        }
        if (ids.size() >= kContent) throw Error("RBF: more than 253 different names");
        if (name.size() > 0xFFFF) throw Error("RBF: a name longer than 65535 characters");
        const uint8_t idx = (uint8_t)ids.size();
        ids.emplace(name, idx);
        put(idx);
        put(type);
        put((uint16_t)name.size());
        raw(name.data(), name.size());
    }
    void value(const RbfValue& v) {
        switch (v.type) {
            case RbfType::U32: put(v.u32); break;
            case RbfType::True: case RbfType::False: break;
            case RbfType::Float: put(v.f[0]); break;
            case RbfType::Float3: for (float x : v.f) put(x); break;
            case RbfType::String:
                if (v.str.size() > 0xFFFF) throw Error("RBF: a string longer than 65535 characters");
                put((uint16_t)v.str.size());
                raw(v.str.data(), v.str.size());
                break;
        }
    }
    void node(const RbfNode& n) {
        switch (n.kind) {
            case RbfNode::Kind::Value:
                token(n.name, (uint8_t)n.value.type);
                value(n.value);
                break;
            case RbfNode::Kind::Content:
                put(kContent);
                put(kClose);
                put((uint32_t)n.content.size());
                raw(n.content.data(), n.content.size());
                break;
            case RbfNode::Kind::Element:
                if (n.attributes.size() > 0xFFFF) throw Error("RBF: more than 65535 attributes on " + n.name);
                token(n.name, 0);
                put(uint32_t(0));
                put((uint16_t)n.attributes.size());
                for (auto& [name, v] : n.attributes) {
                    token(name, (uint8_t)v.type);
                    value(v);
                }
                for (const RbfNode& c : n.children) node(c);
                put(kClose);
                put(kClose);
                break;
        }
    }
};

void dump(const RbfNode& n, int depth, std::string& out, std::string_view array = {}) {
    const std::string pad((size_t)depth * 2, ' ');
    auto val = [](const RbfValue& v) {
        char b[96];
        switch (v.type) {
            case RbfType::U32: std::snprintf(b, sizeof b, "%u", v.u32); return std::string(b);
            case RbfType::True: return std::string("true");
            case RbfType::False: return std::string("false");
            case RbfType::Float: std::snprintf(b, sizeof b, "%g", v.f[0]); return std::string(b);
            case RbfType::Float3: std::snprintf(b, sizeof b, "%g, %g, %g", v.f[0], v.f[1], v.f[2]); return std::string(b);
            case RbfType::String: return v.str;
        }
        return std::string();
    };
    if (n.kind == RbfNode::Kind::Value) {
        out += pad + "<" + n.name + " value=\"" + val(n.value) + "\" />\n";
        return;
    }
    if (n.kind == RbfNode::Kind::Content && (array == "char_array" || array == "short_array")) {
        const size_t w = array == "char_array" ? 1 : 2;
        out += pad;
        for (size_t i = 0; i + w <= n.content.size(); i += w) {
            uint16_t v = n.content[i];
            if (w == 2) std::memcpy(&v, n.content.data() + i, 2);
            out += (i ? " " : "") + std::to_string(v);
        }
        out += '\n';
        return;
    }
    if (n.kind == RbfNode::Kind::Content) {
        bool text = !n.content.empty();
        for (size_t i = 0; i < n.content.size(); i++) text &= n.content[i] >= 32 || (n.content[i] == 0 && i + 1 == n.content.size());
        if (text) out += pad + std::string(n.content.begin(), n.content.end() - (n.content.back() == 0)) + "\n";
        else {
            out += pad + std::to_string(n.content.size()) + " bytes:";
            for (size_t i = 0; i < n.content.size() && i < 32; i++) {
                char h[4];
                std::snprintf(h, sizeof h, " %02x", n.content[i]);
                out += h;
            }
            out += n.content.size() > 32 ? " ...\n" : "\n";
        }
        return;
    }
    out += pad + "<" + n.name;
    for (auto& [name, v] : n.attributes) out += " " + name + "=\"" + val(v) + "\"";
    if (n.children.empty()) {
        out += " />\n";
        return;
    }
    out += ">\n";
    const RbfValue* content = n.attribute("content");
    for (const RbfNode& c : n.children) dump(c, depth + 1, out, content && content->type == RbfType::String ? std::string_view(content->str) : std::string_view());
    out += pad + "</" + n.name + ">\n";
}

}  // namespace

const RbfNode* RbfNode::child(std::string_view name) const {
    for (const RbfNode& c : children)
        if (c.kind != Kind::Content && c.name == name) return &c;
    return nullptr;
}

std::vector<const RbfNode*> RbfNode::children_named(std::string_view name) const {
    std::vector<const RbfNode*> v;
    for (const RbfNode& c : children)
        if (c.kind != Kind::Content && c.name == name) v.push_back(&c);
    return v;
}

const RbfValue* RbfNode::attribute(std::string_view name) const {
    for (auto& [n, v] : attributes)
        if (n == name) return &v;
    return nullptr;
}

std::string RbfNode::text() const {
    for (const RbfNode& c : children)
        if (c.kind == Kind::Content) {
            const size_t n = !c.content.empty() && c.content.back() == 0 ? c.content.size() - 1 : c.content.size();
            return std::string(c.content.begin(), c.content.begin() + (ptrdiff_t)n);
        }
    return {};
}

bool is_rbf(std::span<const uint8_t> file) { return file.size() >= 4 && std::memcmp(file.data(), "RBF0", 4) == 0; }

RbfNode read_rbf(std::span<const uint8_t> file) {
    if (!is_rbf(file)) throw Error("not an RBF file");
    In in{file, 4, {}};
    RbfNode root;
    std::vector<RbfNode*> open;
    for (;;) {
        const uint8_t idx = in.get<uint8_t>(), type = in.get<uint8_t>();
        if (idx == kClose && type == kClose) {
            if (open.empty()) throw Error("RBF: a close with no open element at byte " + std::to_string(in.p - 2));
            open.pop_back();
            if (open.empty()) break;
            continue;
        }
        if (open.empty() && (type != 0 || idx >= kContent)) throw Error("RBF: the file does not start with an element");
        if (idx == kContent && type == kClose) {
            RbfNode c;
            c.kind = RbfNode::Kind::Content;
            const uint32_t n = in.get<uint32_t>();
            in.need(n);
            c.content.assign(file.begin() + (ptrdiff_t)in.p, file.begin() + (ptrdiff_t)(in.p + n));
            in.p += n;
            open.back()->children.push_back(std::move(c));
            continue;
        }
        std::string name = in.name(idx);
        if (type != 0) {
            RbfNode v;
            v.kind = RbfNode::Kind::Value;
            v.name = std::move(name);
            v.value = in.value(type);
            open.back()->children.push_back(std::move(v));
            continue;
        }
        if (in.get<uint32_t>() != 0) throw Error("RBF: element " + name + " has a header value ragekit does not know, at byte " + std::to_string(in.p - 4));
        RbfNode e;
        e.name = std::move(name);
        const uint16_t attrs = in.get<uint16_t>();
        for (uint16_t i = 0; i < attrs; i++) {
            const uint8_t ai = in.get<uint8_t>(), at = in.get<uint8_t>();
            if (ai >= kContent || at == 0) throw Error("RBF: attribute " + std::to_string(i) + " of " + e.name + " is not a value");
            std::string an = in.name(ai);
            e.attributes.emplace_back(std::move(an), in.value(at));
        }
        if (open.empty()) {
            root = std::move(e);
            open.push_back(&root);
        } else {
            // the reading is a loop, but writing, dumping and destroying a tree recurse: elements nested deeper than any
            // game file (a dozen levels) by far are taken as malformed
            if (open.size() >= kMaxDepth) throw Error("RBF: elements nested more than " + std::to_string(kMaxDepth) + " deep, at byte " + std::to_string(in.p));
            open.back()->children.push_back(std::move(e));
            open.push_back(&open.back()->children.back());
        }
    }
    if (in.p != file.size()) throw Error("RBF: " + std::to_string(file.size() - in.p) + " bytes after the root element");
    return root;
}

Bytes write_rbf(const RbfNode& root) {
    if (root.kind != RbfNode::Kind::Element) throw Error("RBF: the root must be an element");
    Out out;
    out.raw("RBF0", 4);
    out.node(root);
    return out.b;
}

std::string rbf_dump(const RbfNode& root) {
    std::string s;
    dump(root, 0, s);
    return s;
}

}  // namespace rk
