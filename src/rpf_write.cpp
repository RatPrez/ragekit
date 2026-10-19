// RPF7 writer. Layout rules ported from CodeWalker's RpfFile.cs (EnsureAllEntries, GetHeaderNamesData,
// WriteHeader, CreateFile): see rpf.h.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include "ragekit/error.h"
#include "ragekit/rpf.h"
#include "ragekit/rsc.h"
#include "rpf_crypto.h"

namespace rk {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
bool ends_with(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && std::equal(s.end() - (ptrdiff_t)n, s.end(), suffix);
}
std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
} // namespace

struct RpfBuilder::Node {
    enum class Kind { Dir, Binary, Resource, Archive };
    Kind kind = Kind::Dir;
    std::string name;
    std::vector<std::shared_ptr<Node>> kids;

    // Content, one of: in memory (`data`), copied lazily from `src`, or a child builder.
    Bytes data;               // binary: the bytes to store (before compress if `compress`); resource: the RSC7 file
    bool compress = false;    // deflate `data` when writing
    std::shared_ptr<const Rpf> src;
    int src_idx = -1;
    std::shared_ptr<RpfBuilder> child;

    Node* find(const std::string& n) const {
        std::string l = lower(n);
        for (auto& k : kids) if (lower(k->name) == l) return k.get();
        return nullptr;
    }
};

using Node = RpfBuilder::Node;

RpfBuilder::RpfBuilder() : root_(std::make_shared<Node>()) {}

static std::shared_ptr<Node> clone_node(const Node& n) {
    auto c = std::make_shared<Node>(n);
    for (auto& k : c->kids) k = clone_node(*k);
    if (n.child) c->child = std::make_shared<RpfBuilder>(n.child->clone());
    return c;
}
RpfBuilder RpfBuilder::clone() const {
    RpfBuilder b;
    b.root_ = clone_node(*root_);
    return b;
}

// ---- tree editing -----------------------------------------------------------------------

static Node* ensure_dir(Node* root, const std::vector<std::string>& parts, size_t count) {
    Node* cur = root;
    for (size_t i = 0; i < count; i++) {
        Node* next = cur->find(parts[i]);
        if (!next) {
            auto n = std::make_shared<Node>();
            n->kind = Node::Kind::Dir;
            n->name = parts[i];
            cur->kids.push_back(n);
            next = n.get();
        } else if (next->kind != Node::Kind::Dir)
            throw Error("'" + parts[i] + "' is a file, not a directory");
        cur = next;
    }
    return cur;
}

static Node* put_file(Node* root, const std::string& path, Node::Kind kind) {
    auto parts = split_path(path);
    if (parts.empty()) throw Error("empty entry path");
    Node* dir = ensure_dir(root, parts, parts.size() - 1);
    const std::string& name = parts.back();
    if (name.size() > 256) throw Error("entry name too long: " + name);
    if (Node* old = dir->find(name)) {
        if (old->kind == Node::Kind::Dir) throw Error("'" + path + "' is a directory");
        dir->kids.erase(std::remove_if(dir->kids.begin(), dir->kids.end(), [&](auto& k) { return k.get() == old; }), dir->kids.end());
    }
    auto n = std::make_shared<Node>();
    n->kind = kind;
    n->name = name;
    dir->kids.push_back(n);
    return n.get();
}

void RpfBuilder::add_directory(const std::string& path) {
    auto parts = split_path(path);
    ensure_dir(root_.get(), parts, parts.size());
}

void RpfBuilder::add_binary(const std::string& path, Bytes data, bool compress) {
    Node* n = put_file(root_.get(), path, Node::Kind::Binary);
    std::string l = lower(n->name);
    n->compress = compress && !ends_with(l, ".rpf") && !ends_with(l, ".awc") && !data.empty();
    n->data = std::move(data);
}

void RpfBuilder::add_resource(const std::string& path, Bytes file) {
    if (file.size() < 16 || !rsc::Resource::is_resource(file)) throw Error("'" + path + "' is not an RSC7 file");
    Node* n = put_file(root_.get(), path, Node::Kind::Resource);
    n->data = std::move(file);
}

void RpfBuilder::add_file(const std::string& path, Bytes data) {
    if (data.size() >= 16 && rsc::Resource::is_resource(data)) add_resource(path, std::move(data));
    else add_binary(path, std::move(data), true);
}

void RpfBuilder::add_archive(const std::string& path, RpfBuilder child) {
    Node* n = put_file(root_.get(), path, Node::Kind::Archive);
    n->child = std::make_shared<RpfBuilder>(std::move(child));
}

static Node* resolve(Node* root, const std::string& path) {
    Node* cur = root;
    for (auto& p : split_path(path)) {
        if (cur->kind != Node::Kind::Dir) return nullptr;
        cur = cur->find(p);
        if (!cur) return nullptr;
    }
    return cur;
}

void RpfBuilder::add_stored(const std::string& path, const Rpf& rpf, int entry) {
    const RpfEntry& e = rpf.entries().at((size_t)entry);
    if (e.is_directory()) throw Error("'" + e.path + "' is a directory");
    Node* n = put_file(root_.get(), path, e.is_resource() ? Node::Kind::Resource : Node::Kind::Binary);
    n->src = std::make_shared<Rpf>(rpf);
    n->src_idx = entry;
}

Bytes RpfBuilder::read(const std::string& path) const {
    const Node* n = resolve(root_.get(), path);
    if (!n) throw Error("no entry '" + path + "' in the archive being built");
    switch (n->kind) {
        case Node::Kind::Dir: throw Error("'" + path + "' is a directory");
        case Node::Kind::Archive: return n->child->build();
        default: return n->src ? n->src->extract(n->src_idx) : n->data;
    }
}

bool RpfBuilder::exists(const std::string& path) const { return resolve(root_.get(), path) != nullptr; }
bool RpfBuilder::is_directory(const std::string& path) const {
    Node* n = resolve(root_.get(), path);
    return n && n->kind == Node::Kind::Dir;
}

bool RpfBuilder::remove(const std::string& path) {
    auto parts = split_path(path);
    if (parts.empty()) return false;
    Node* dir = resolve(root_.get(), "");
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        dir = dir->find(parts[i]);
        if (!dir || dir->kind != Node::Kind::Dir) return false;
    }
    Node* n = dir->find(parts.back());
    if (!n) return false;
    dir->kids.erase(std::remove_if(dir->kids.begin(), dir->kids.end(), [&](auto& k) { return k.get() == n; }), dir->kids.end());
    return true;
}

bool RpfBuilder::rename(const std::string& path, const std::string& new_name) {
    auto parts = split_path(path);
    if (parts.empty()) return false;
    if (new_name.empty() || new_name.find_first_of("/\\") != std::string::npos) throw Error("bad entry name: " + new_name);
    Node* n = resolve(root_.get(), path);
    if (!n) return false;
    Node* dir = root_.get();
    for (size_t i = 0; i + 1 < parts.size(); i++) dir = dir->find(parts[i]);
    Node* clash = dir->find(new_name);
    if (clash && clash != n) throw Error("'" + new_name + "' already exists");
    n->name = new_name;
    return true;
}

RpfBuilder* RpfBuilder::nested(const std::string& path) {
    Node* n = resolve(root_.get(), path);
    return n && n->kind == Node::Kind::Archive ? n->child.get() : nullptr;
}

static void list_node(const Node& n, const std::string& prefix, std::vector<std::string>& out) {
    std::vector<const Node*> kids;
    for (auto& k : n.kids) kids.push_back(k.get());
    std::sort(kids.begin(), kids.end(), [](const Node* a, const Node* b) { return a->name < b->name; });
    for (auto* k : kids) {
        std::string p = prefix.empty() ? k->name : prefix + "/" + k->name;
        out.push_back(p);
        list_node(*k, p, out);
    }
}
std::vector<std::string> RpfBuilder::list() const {
    std::vector<std::string> out;
    list_node(*root_, "", out);
    return out;
}

// ---- loading an existing archive --------------------------------------------------------

static bool is_encrypted(RpfEncryption e) { return e != RpfEncryption::None && e != RpfEncryption::Open; }

static void load_dir(const std::shared_ptr<const Rpf>& rpf, int dir, Node& into, RpfBuilder::Nested mode) {
    for (int c : rpf->children(dir)) {
        const RpfEntry& e = rpf->entries()[(size_t)c];
        auto n = std::make_shared<Node>();
        n->name = e.name;
        if (e.is_directory()) {
            n->kind = Node::Kind::Dir;
            load_dir(rpf, c, *n, mode);
        } else if (e.is_resource()) {
            n->kind = Node::Kind::Resource;
            n->src = rpf;
            n->src_idx = c;
        } else if (e.is_archive() && (mode == RpfBuilder::Nested::Rewrite || is_encrypted(rpf->open_child(c).encryption()))) {
            n->kind = Node::Kind::Archive;
            n->child = std::make_shared<RpfBuilder>(RpfBuilder::from_archive(rpf->open_child(c), mode));
        } else {
            n->kind = Node::Kind::Binary;
            n->src = rpf;
            n->src_idx = c;
        }
        into.kids.push_back(n);
    }
}

RpfBuilder RpfBuilder::from_archive(const Rpf& rpf, Nested mode) {
    RpfBuilder b;
    load_dir(std::make_shared<Rpf>(rpf), 0, *b.root_, mode);
    return b;
}

// ---- writing ----------------------------------------------------------------------------

namespace {

struct Prepared {
    Bytes data;                 // what goes into the file (resource: whole RSC7 file)
    uint32_t stored = 0;        // binary: size field (0 = stored uncompressed)
    uint32_t uncompressed = 0;  // binary
    uint32_t sys = 0, gfx = 0;  // resource
    bool resource = false;
    bool encrypted = false;     // binary: entry flag (only set when copied from a source that says so and we kept the bytes encrypted)
};

struct Flat {
    const Node* node;
    int parent;
    uint32_t first = 0, count = 0;
    uint32_t name_offset = 0;
};

Prepared prepare(const Node& n, const RpfWriteOptions& opt);

Bytes build_archive(const Node& root, const RpfWriteOptions& opt);

Prepared prepare(const Node& n, const RpfWriteOptions& opt) {
    Prepared p;
    if (n.kind == Node::Kind::Archive) {
        RpfWriteOptions child = opt;
        child.name = n.name;   // a nested archive's NG key comes from its own name
        p.data = build_archive(*n.child->root_node(), child);
        p.uncompressed = (uint32_t)p.data.size();
        return p;
    }
    if (n.src) {
        const RpfEntry& e = n.src->entries()[(size_t)n.src_idx];
        if (e.is_resource()) {
            p.resource = true;
            p.sys = e.system_flags;
            p.gfx = e.graphics_flags;
            // .ysc files stay encrypted in the source scheme (their key is the name and size: unchanged here)
            p.data = e.encrypted ? n.src->extract_raw(n.src_idx) : n.src->extract_stored(n.src_idx);
        } else {
            p.data = n.src->extract_stored(n.src_idx);
            p.uncompressed = e.uncompressed_size;
            p.stored = e.compressed() ? (uint32_t)p.data.size() : 0;
            if (!e.compressed()) p.uncompressed = (uint32_t)p.data.size();
        }
        return p;
    }
    if (n.kind == Node::Kind::Resource) {
        p.resource = true;
        p.data = n.data;
        std::memcpy(&p.sys, p.data.data() + 8, 4);
        std::memcpy(&p.gfx, p.data.data() + 12, 4);
        return p;
    }
    p.uncompressed = (uint32_t)n.data.size();
    if (n.compress && opt.compress) {
        Bytes z = rsc::deflate(n.data);
        if (z.size() < n.data.size() && z.size() <= 0xFFFFFF) {
            p.data = std::move(z);
            p.stored = (uint32_t)p.data.size();
            return p;
        }
    }
    p.data = n.data;
    return p;
}

void write_zeros(std::ostream& os, uint64_t n) {
    static const char z[4096] = {};
    while (n) {
        uint64_t k = std::min<uint64_t>(n, sizeof z);
        os.write(z, (std::streamsize)k);
        n -= k;
    }
}

void write_archive(const Node& root, const RpfWriteOptions& opt, std::ostream& os) {
    std::shared_ptr<const RpfKeys> keys = opt.keys;
    if (opt.encryption == RpfEncryption::Aes) {
        if (!keys) keys = RpfKeys::default_keys();
        if (!keys->has_aes()) throw Error("no AES key available for an AES archive (see RpfKeys)");
    }
    if (opt.encryption == RpfEncryption::Ng) {
        if (!keys) keys = RpfKeys::default_keys();
        if (!keys->has_ng()) throw Error("no NG keys available for an NG archive (see RpfKeys)");
        if (opt.name.empty()) throw Error("an NG archive needs its file name (RpfWriteOptions::name): its key depends on it");
        if (!rpfcrypto::ng_can_encrypt(keys->ng_tables.data())) throw Error("the NG tables do not invert");
    }

    // table order: a directory's children are contiguous and sorted by name; directories get their
    // children in depth first preorder (what the sapo pack and retail archives have; CodeWalker's
    // EnsureAllEntries pops a stack and so visits sibling directories in reverse, the game does not care)
    std::vector<Flat> flat;
    flat.push_back({&root, -1});
    std::vector<uint32_t> pending; // directories waiting for their children, in preorder
    std::function<void(int)> expand = [&](int di) {
        std::vector<const Node*> kids;
        for (auto& k : flat[(size_t)di].node->kids) kids.push_back(k.get());
        std::sort(kids.begin(), kids.end(), [](const Node* a, const Node* b) { return a->name < b->name; });
        flat[(size_t)di].first = (uint32_t)flat.size();
        flat[(size_t)di].count = (uint32_t)kids.size();
        for (const Node* k : kids) flat.push_back({k, di});
    };
    // preorder with an explicit stack: children pushed in reverse so the first directory is expanded first
    {
        std::vector<int> stack{0};
        while (!stack.empty()) {
            int di = stack.back();
            stack.pop_back();
            expand(di);
            uint32_t f = flat[(size_t)di].first, c = flat[(size_t)di].count;
            for (uint32_t i = c; i-- > 0;)
                if (flat[f + i].node->kind == Node::Kind::Dir) stack.push_back((int)(f + i));
        }
    }

    // names, deduplicated, in table order
    Bytes names;
    std::unordered_map<std::string, uint32_t> seen;
    for (auto& f : flat) {
        auto it = seen.find(f.node->name);
        if (it != seen.end()) { f.name_offset = it->second; continue; }
        f.name_offset = (uint32_t)names.size();
        seen[f.node->name] = f.name_offset;
        names.insert(names.end(), f.node->name.begin(), f.node->name.end());
        names.push_back(0);
    }
    while (names.size() % 16) names.push_back(0);
    uint64_t header_used = 16 + (uint64_t)flat.size() * 16 + names.size();
    uint64_t header_blocks = (header_used + 511) / 512;

    write_zeros(os, header_blocks * 512);
    Bytes table(flat.size() * 16, 0);
    uint64_t block = header_blocks;
    for (size_t i = 0; i < flat.size(); i++) {
        const Flat& f = flat[i];
        uint8_t* e = table.data() + i * 16;
        if (f.node->kind == Node::Kind::Dir) {
            put32(e, f.name_offset);
            put32(e + 4, 0x7FFFFF00u);
            put32(e + 8, f.first);
            put32(e + 12, f.count);
            continue;
        }
        if (f.name_offset > 0xFFFF) throw Error("names table too large for '" + f.node->name + "'");
        Prepared p = prepare(*f.node, opt);
        uint64_t len = p.data.size();
        if (p.resource) {
            if (len < 16) throw Error("resource '" + f.node->name + "' is smaller than its header");
            uint32_t size_field = (uint32_t)len;
            if (len >= 0xFFFFFF) {
                // sizes of 0xFFFFFF mean "at least": the real size is scrambled into the 16 byte header
                uint32_t l = (uint32_t)len;
                p.data[7] = (uint8_t)(l >> 0); p.data[14] = (uint8_t)(l >> 8); p.data[5] = (uint8_t)(l >> 16); p.data[2] = (uint8_t)(l >> 24);
                size_field = 0xFFFFFF;
            }
            if (block > 0x7FFFFF) throw Error("archive too large for resource offsets");
            e[0] = (uint8_t)f.name_offset; e[1] = (uint8_t)(f.name_offset >> 8);
            e[2] = (uint8_t)size_field; e[3] = (uint8_t)(size_field >> 8); e[4] = (uint8_t)(size_field >> 16);
            e[5] = (uint8_t)block; e[6] = (uint8_t)(block >> 8); e[7] = (uint8_t)((block >> 16) | 0x80);
            put32(e + 8, p.sys);
            put32(e + 12, p.gfx);
        } else {
            if (block > 0xFFFFFF) throw Error("archive too large for file offsets");
            e[0] = (uint8_t)f.name_offset; e[1] = (uint8_t)(f.name_offset >> 8);
            e[2] = (uint8_t)p.stored; e[3] = (uint8_t)(p.stored >> 8); e[4] = (uint8_t)(p.stored >> 16);
            e[5] = (uint8_t)block; e[6] = (uint8_t)(block >> 8); e[7] = (uint8_t)(block >> 16);
            put32(e + 8, p.uncompressed);
            put32(e + 12, p.encrypted ? 1 : 0);
        }
        if (len) os.write((const char*)p.data.data(), (std::streamsize)len);
        uint64_t blocks = (len + 511) / 512;
        write_zeros(os, blocks * 512 - len);
        block += blocks;
    }

    Bytes hdr_names = names;
    if (opt.encryption == RpfEncryption::Aes) {
        rpfcrypto::aes_encrypt(table.data(), table.size(), keys->aes.data());
        rpfcrypto::aes_encrypt(hdr_names.data(), hdr_names.size(), keys->aes.data());
    } else if (opt.encryption == RpfEncryption::Ng) {
        // the key: the archive's name and its whole size, as the reader takes them
        const uint32_t length = (uint32_t)(block * 512);
        for (Bytes* b : {&table, &hdr_names})
            rpfcrypto::ng_encrypt(b->data(), b->size(), opt.name, length, keys->ng_keys.data(), keys->ng_tables.data(), keys->hash_lut.data());
    }
    uint8_t head[16];
    uint32_t enc = opt.encryption == RpfEncryption::None ? 0u : (uint32_t)opt.encryption;
    put32(head, 0x52504637);
    put32(head + 4, (uint32_t)flat.size());
    put32(head + 8, (uint32_t)names.size());
    put32(head + 12, enc);
    os.seekp(0);
    os.write((const char*)head, 16);
    os.write((const char*)table.data(), (std::streamsize)table.size());
    os.write((const char*)hdr_names.data(), (std::streamsize)hdr_names.size());
}

Bytes build_archive(const Node& root, const RpfWriteOptions& opt) {
    std::ostringstream os(std::ios::binary);
    write_archive(root, opt, os);
    std::string s = os.str();
    return Bytes(s.begin(), s.end());
}

} // namespace

Bytes RpfBuilder::build(const RpfWriteOptions& opt) const { return build_archive(*root_, opt); }

void RpfBuilder::save(const std::string& path, const RpfWriteOptions& opt_in) const {
    RpfWriteOptions opt = opt_in;
    if (opt.name.empty()) opt.name = std::filesystem::path(path).filename().string();
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) throw Error("cannot write " + path);
    write_archive(*root_, opt, os);
    os.flush();
    if (!os) throw Error("write failed: " + path);
}

} // namespace rk
