// ragekit - game asset index. See include/ragekit/gameindex.h.
//
// Cache file (%USERPROFILE%\.ragekit\index\<key>.bin), format version GameIndex::kFormat, little endian:
//   "RKGI" u32 format | u64 key | f64 scan_seconds | str game_dir | str exe_version
//   vec<str> extensions
//   archives: u32 n, { str path, i32 parent, u32 parent_entry, u32 file_count }
//   dirs: pool (u32 n, u32 offsets[n+1], u32 blob_len, blob)        directory path inside its archive
//   ranks: u32 n, { i32 tier, i32 order, i32 index, i32 patch, str dlc }
//   files: u32 n, n * IndexedFile (24 bytes)                         sorted by (name_hash, ext, rank, archive, entry)
//   archetypes: u32 n, n * IndexedArchetype (raw)                    sorted by (name, rank)
//   names: u32 n, u32 hashes[n] (ascending), pool of the strings     file name stems, lower case
//   txd parents: u32 n, n * { u32 child, u32 parent }                sorted by child
//   vec<str> warnings
// "str" = u32 length + bytes.
#include "ragekit/gameindex.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include "ragekit/error.h"
#include "ragekit/hash.h"
#include "ragekit/meta.h"
#include "ragekit/rsc.h"

namespace fs = std::filesystem;

namespace rk {

static_assert(sizeof(IndexedFile) == 28);
static_assert(std::is_trivially_copyable_v<IndexedFile>);
static_assert(std::is_trivially_copyable_v<IndexedArchetype>);

namespace {

struct ParentRec { uint32_t child, parent; };

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string& s, const char* e) {
    size_t n = std::strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}

std::string home_dir() {
    const char* h = std::getenv("USERPROFILE");
    if (!h || !*h) h = std::getenv("HOME");
    return h ? h : ".";
}

// ---------------------------------------------------------------------------------------
// String pool

struct Pool {
    std::string blob;
    std::vector<uint32_t> off{0};
    uint32_t add(const std::string& s) {
        blob += s;
        off.push_back((uint32_t)blob.size());
        return (uint32_t)off.size() - 2;
    }
    size_t size() const { return off.size() - 1; }
    std::string_view get(size_t i) const { return std::string_view(blob).substr(off[i], off[i + 1] - off[i]); }
};

// ---------------------------------------------------------------------------------------
// Binary writer / reader for the cache

struct Writer {
    Bytes b;
    void raw(const void* p, size_t n) { const auto* c = (const uint8_t*)p; b.insert(b.end(), c, c + n); }
    template <class T> void pod(const T& v) { raw(&v, sizeof(T)); }
    void str(const std::string& s) { pod((uint32_t)s.size()); raw(s.data(), s.size()); }
    template <class T> void vec(const std::vector<T>& v) { pod((uint32_t)v.size()); raw(v.data(), v.size() * sizeof(T)); }
    void pool(const Pool& p) { vec(p.off); pod((uint32_t)p.blob.size()); raw(p.blob.data(), p.blob.size()); }
};

struct CacheReader {
    const uint8_t* p;
    const uint8_t* end;
    void need(size_t n) const { if ((size_t)(end - p) < n) throw Error("index cache is truncated"); }
    template <class T> T pod() { need(sizeof(T)); T v; std::memcpy(&v, p, sizeof(T)); p += sizeof(T); return v; }
    std::string str() { uint32_t n = pod<uint32_t>(); need(n); std::string s((const char*)p, n); p += n; return s; }
    template <class T> std::vector<T> vec() {
        uint32_t n = pod<uint32_t>();
        need((size_t)n * sizeof(T));
        std::vector<T> v(n);
        if (n) std::memcpy(v.data(), p, (size_t)n * sizeof(T));
        p += (size_t)n * sizeof(T);
        return v;
    }
    Pool pool() {
        Pool q;
        q.off = vec<uint32_t>();
        uint32_t n = pod<uint32_t>();
        need(n);
        q.blob.assign((const char*)p, n);
        p += n;
        if (q.off.empty() || q.off.back() != q.blob.size()) throw Error("index cache: bad string pool");
        return q;
    }
};

uint64_t fnv(uint64_t h, const std::string& s) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    h ^= 0xff; h *= 1099511628211ull;
    return h;
}

// ---------------------------------------------------------------------------------------
// Small text/binary parsers for the data files the load order needs

/// Content of every <Item> in dlclist.xml, in order (tag name case-insensitive).
std::vector<std::string> xml_items(const std::string& xml) {
    std::string low = lower(xml);
    std::vector<std::string> out;
    size_t pos = 0;
    while ((pos = low.find("<item>", pos)) != std::string::npos) {
        size_t s = pos + 6, e = low.find("</item>", s);
        if (e == std::string::npos) break;
        std::string t = xml.substr(s, e - s);
        size_t a = t.find_first_not_of(" \t\r\n"), z = t.find_last_not_of(" \t\r\n");
        out.push_back(a == std::string::npos ? "" : t.substr(a, z - a + 1));
        pos = e + 7;
    }
    return out;
}

int setup_order(const std::string& xml) {
    std::string low = lower(xml);
    size_t p = low.find("<order");
    if (p == std::string::npos) return 0;
    size_t v = low.find("value=\"", p);
    size_t e = low.find('>', p);
    if (v == std::string::npos || v > e) return 0;
    return std::atoi(low.c_str() + v + 7);
}

using TxdPairs = std::vector<std::pair<std::string, std::string>>; // child, parent

/// gtxd.meta (XML): <item><parent>x</parent><child>y</child></item>; vehicles.meta's <txdRelationships> alike
TxdPairs gtxd_xml(const std::string& xml) {
    TxdPairs out;
    std::string low = lower(xml);
    size_t pos = 0;
    auto grab = [&](const char* tag, size_t from, size_t& endpos) -> std::string {
        std::string o = std::string("<") + tag + ">", c = std::string("</") + tag + ">";
        size_t s = low.find(o, from);
        if (s == std::string::npos) { endpos = std::string::npos; return {}; }
        s += o.size();
        size_t e = low.find(c, s);
        if (e == std::string::npos) { endpos = std::string::npos; return {}; }
        endpos = e + c.size();
        std::string t = xml.substr(s, e - s);
        size_t a = t.find_first_not_of(" \t\r\n"), z = t.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? "" : t.substr(a, z - a + 1);
    };
    for (;;) {
        size_t e1, e2;
        std::string parent = grab("parent", pos, e1);
        if (e1 == std::string::npos) break;
        std::string child = grab("child", e1, e2);
        if (e2 == std::string::npos) break;
        pos = e2;
        if (!parent.empty() && !child.empty()) out.emplace_back(child, parent);
    }
    return out;
}

/// gtxd.ymt: an RBF file (CodeWalker's Rbf.cs): tagged tree, the txd names are byte blobs inside <parent>/<child> elements.
TxdPairs gtxd_rbf(const Bytes& d) {
    TxdPairs out;
    size_t p = 4;
    auto u8 = [&]() -> uint8_t { if (p >= d.size()) throw Error("rbf: truncated"); return d[p++]; };
    auto skip = [&](size_t n) { if (p + n > d.size()) throw Error("rbf: truncated"); p += n; };
    auto i16 = [&]() -> int { int v = u8(); v |= u8() << 8; return v; };
    auto i32 = [&]() -> uint32_t { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)u8() << (8 * i); return v; };
    uint32_t ident = d.size() >= 4 ? (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24 : 0;
    if (ident != 0x30464252) throw Error("not an RBF file");
    std::vector<std::string> names, stack;
    std::string parent, child;
    while (p < d.size()) {
        uint8_t idx = u8();
        if (idx == 0xFF) {
            u8();
            if (stack.empty()) return out;
            std::string closed = stack.back();
            stack.pop_back();
            if (lower(closed) == "item") {
                if (!parent.empty() && !child.empty()) out.emplace_back(child, parent);
                parent.clear(); child.clear();
            }
        } else if (idx == 0xFD) {
            u8();
            uint32_t n = i32();
            if (p + n > d.size()) throw Error("rbf: truncated");
            std::string s((const char*)d.data() + p, n);
            p += n;
            s.erase(std::remove(s.begin(), s.end(), '\0'), s.end());
            if (!stack.empty()) {
                std::string top = lower(stack.back());
                if (top == "parent") parent = s;
                else if (top == "child") child = s;
            }
        } else {
            uint8_t type = u8();
            if (idx == names.size()) {
                int n = i16();
                if (p + (size_t)n > d.size()) throw Error("rbf: truncated");
                names.emplace_back((const char*)d.data() + p, (size_t)n);
                p += (size_t)n;
            } else if (idx > names.size()) throw Error("rbf: bad descriptor");
            switch (type) {
                case 0: stack.push_back(names[idx]); skip(6); break;
                case 0x10: skip(4); break;
                case 0x20: case 0x30: break;
                case 0x40: skip(4); break;
                case 0x50: skip(12); break;
                case 0x60: { int n = i16(); skip((size_t)n); break; }
                default: throw Error("rbf: unsupported data type");
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// Load order

struct Pack {
    std::string name, prefix; // prefix: lower case, '/' terminated, e.g. "update/x64/dlcpacks/mpheist/"
    int list_index = -1, order = 0, mount = -1;
    bool listed() const { return list_index >= 0; }
};

struct Packs {
    std::vector<Pack> all;
    std::unordered_map<std::string, size_t> by_prefix, by_name;
};

LoadRank rank_of(const std::string& p, const Packs& packs) {
    LoadRank r;
    size_t start = std::string::npos;
    static const std::string upk = "update/x64/dlcpacks/";
    if (starts_with(p, upk)) start = upk.size();
    else if (starts_with(p, "x64")) {
        size_t rpf = p.find(".rpf/dlcpacks/");
        if (rpf != std::string::npos && p.find('/') == rpf + 4) start = rpf + 4 + 10; // "/dlcpacks/"
    }
    if (start != std::string::npos && start < p.size()) {
        size_t end = p.find('/', start);
        std::string prefix = end == std::string::npos ? p : p.substr(0, end + 1);
        auto it = packs.by_prefix.find(prefix);
        if (it != packs.by_prefix.end()) {
            const Pack& k = packs.all[it->second];
            if (k.listed()) { r.tier = 3; r.order = k.order; r.index = k.mount; r.dlc = k.name; }
            else { r.tier = -1; r.dlc = k.name; }
        } else {
            r.tier = -1;
            r.dlc = lower(end == std::string::npos ? p.substr(start) : p.substr(start, end - start));
        }
        return r;
    }
    if (starts_with(p, "update/update2.rpf")) { r.tier = 2; return r; }
    static const std::string patch = "update/update.rpf/dlc_patch/";
    if (starts_with(p, patch) && p.size() > patch.size()) {
        size_t end = p.find('/', patch.size());
        std::string name = lower(end == std::string::npos ? p.substr(patch.size()) : p.substr(patch.size(), end - patch.size()));
        auto it = packs.by_name.find(name);
        r.patch = 1;
        r.dlc = name;
        if (it != packs.by_name.end() && packs.all[it->second].listed()) {
            const Pack& k = packs.all[it->second];
            r.tier = 3; r.order = k.order; r.index = k.mount;
        } else r.tier = -1;
        return r;
    }
    if (starts_with(p, "update/update.rpf")) { r.tier = 1; return r; }
    return r;
}

bool rank_less(const LoadRank& a, const LoadRank& b) {
    if (a.tier != b.tier) return a.tier < b.tier;
    if (a.order != b.order) return a.order < b.order;
    if (a.index != b.index) return a.index < b.index;
    if (a.patch != b.patch) return a.patch < b.patch;
    return a.dlc < b.dlc;
}
bool rank_eq(const LoadRank& a, const LoadRank& b) { return !rank_less(a, b) && !rank_less(b, a); }

// ---------------------------------------------------------------------------------------
// Scan

struct Scan {
    std::vector<IndexedArchive> archives;
    std::vector<IndexedFile> files;       // archive/dir indices are local to this scan
    std::vector<std::string> dirs;
    std::vector<uint32_t> dir_archive;    // owner (local archive index) of each dir
    std::vector<std::string> exts;
    std::unordered_map<std::string, uint16_t> ext_ids;
    std::vector<IndexedArchetype> archs;  // ytyp_file is a local file index
    std::unordered_map<std::string, uint32_t> stem_ids; // file name stems of this scan
    std::vector<std::string> stems;
    std::vector<std::string> warnings;
    struct Gtxd { uint32_t file; TxdPairs pairs; };
    std::vector<Gtxd> gtxd;
    std::string dlclist;
    std::vector<std::pair<uint32_t, int>> setup; // local archive index (a dlc*.rpf), <order>

    uint16_t ext_id(const std::string& e) {
        auto it = ext_ids.find(e);
        if (it != ext_ids.end()) return it->second;
        uint16_t id = (uint16_t)exts.size();
        exts.push_back(e);
        ext_ids.emplace(e, id);
        return id;
    }
};

bool is_dlc_archive_name(const std::string& lname) {
    return lname.size() >= 7 && starts_with(lname, "dlc") && ends_with(lname, ".rpf") && lname.size() <= 12;
}

void read_ytyp_into(Scan& s, const Rpf& rpf, int entry_index, const std::string& where, uint32_t file_local) {
    Bytes data;
    try { data = rpf.extract(entry_index); }
    catch (const std::exception& ex) { s.warnings.push_back(where + ": cannot extract: " + ex.what()); return; }
    if (data.size() >= 4 && std::memcmp(data.data(), "PSIN", 4) == 0) {
        s.warnings.push_back(where + ": PSO file, not a meta .ytyp; its archetypes are not indexed (CodeWalker cannot read it either)");
        return;
    }
    try {
        MapTypes t = read_ytyp(data);
        for (const Archetype& a : t.archetypes) {
            if (a.kind == Archetype::Kind::Other) {
                // older CBaseArchetypeDef (128 bytes, destruction props): no asset type / asset name, the model is named like the archetype.
                // The typed layer keeps these untouched; read the fields the index needs from the detached bytes.
                if (a.other.type == 0x82D6FC83 && a.other.data.size() == 128) {
                    const uint8_t* b = a.other.data.data();
                    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, b + o, 4); return v; };
                    auto f32 = [&](size_t o) { float v; std::memcpy(&v, b + o, 4); return v; };
                    IndexedArchetype r{};
                    r.name = u32(88);
                    r.texture_dictionary = u32(92);
                    r.clip_dictionary = u32(96);
                    r.drawable_dictionary = u32(100);
                    r.physics_dictionary = u32(104);
                    r.flags = u32(12);
                    r.ytyp_name = t.name;
                    r.ytyp_file = file_local;
                    r.lod_dist = f32(8);
                    r.bb_min = Vec3(f32(32), f32(36), f32(40));
                    r.bb_max = Vec3(f32(48), f32(52), f32(56));
                    r.kind = IndexedArchetype::Base;
                    r.asset_type = 0; // unknown: the old structure has none
                    s.archs.push_back(r);
                } else {
                    s.warnings.push_back(where + ": archetype of unknown structure " + hash_name(a.other.type) + " (" + std::to_string(a.other.data.size()) + " bytes) skipped");
                }
                continue;
            }
            IndexedArchetype r{};
            r.name = a.name;
            r.asset_name = a.asset_name;
            r.texture_dictionary = a.texture_dictionary;
            r.drawable_dictionary = a.drawable_dictionary;
            r.physics_dictionary = a.physics_dictionary;
            r.clip_dictionary = a.clip_dictionary;
            r.flags = a.flags;
            r.ytyp_name = t.name;
            r.ytyp_file = file_local;
            r.lod_dist = a.lod_dist;
            r.bb_min = a.bb_min;
            r.bb_max = a.bb_max;
            r.kind = a.kind == Archetype::Kind::Time ? IndexedArchetype::Time : a.kind == Archetype::Kind::Mlo ? IndexedArchetype::Mlo : IndexedArchetype::Base;
            r.asset_type = (int8_t)a.asset_type;
            s.archs.push_back(r);
        }
    } catch (const std::exception& ex) {
        std::string note = where + ": typed read failed (" + ex.what() + ")";
        try { read_meta(data); note += "; generic read ok, skipped"; }
        catch (const std::exception& ex2) { note += "; generic read failed too (" + std::string(ex2.what()) + "), skipped"; }
        s.warnings.push_back(note);
    }
}

void scan_archive(Scan& s, const Rpf& rpf, const std::string& path, int32_t parent, uint32_t parent_entry, bool top_update) {
    uint32_t ai = (uint32_t)s.archives.size();
    s.archives.push_back({path, parent, parent_entry, 0});
    const auto& es = rpf.entries();
    std::vector<uint32_t> dir_of(es.size(), 0);
    for (size_t i = 0; i < es.size(); i++)
        if (es[i].is_directory()) { dir_of[i] = (uint32_t)s.dirs.size(); s.dirs.push_back(es[i].path); s.dir_archive.push_back(ai); }
    std::string lname_archive = lower(rpf.name());
    uint32_t count = 0;
    for (size_t i = 1; i < es.size(); i++) {
        const RpfEntry& e = es[i];
        if (e.is_directory()) continue;
        std::string lname = lower(e.name);
        size_t dot = lname.rfind('.');
        std::string stem = dot != std::string::npos && dot > 0 ? lname.substr(0, dot) : lname;
        std::string ext = dot != std::string::npos && dot > 0 ? lname.substr(dot) : std::string();
        uint32_t h = joaat_lower(stem);
        auto ins = s.stem_ids.try_emplace(stem, (uint32_t)s.stems.size());
        if (ins.second) s.stems.push_back(stem);

        IndexedFile f{};
        f.name_hash = h;
        f.name_id = ins.first->second;
        f.archive = ai;
        f.entry = (uint32_t)i;
        f.dir = dir_of[e.parent >= 0 ? (size_t)e.parent : 0];
        f.size = (uint32_t)std::min<uint64_t>(e.size(), 0xFFFFFFFFull);
        f.ext = s.ext_id(ext);
        uint32_t file_local = (uint32_t)s.files.size();
        s.files.push_back(f);
        count++;
        std::string where = path + "/" + e.path;

        try {
            if (e.is_archive()) {
                Rpf child = rpf.open_child((int)i);
                scan_archive(s, child, where, (int32_t)ai, (uint32_t)i, false);
            } else if (ext == ".ytyp") {
                read_ytyp_into(s, rpf, (int)i, where, file_local);
            } else if (lname == "gtxd.ymt" || lname == "mph4_gtxd.ymt" || lname == "gtxd.meta" || lname == "vehicles.meta") {
                // vehicles.meta: its <txdRelationships> are the same <parent>/<child> pairs (vehicle txds over vehshare)
                Bytes data = rpf.extract((int)i);
                TxdPairs pairs;
                if (data.size() >= 4 && data[0] == 'R' && data[1] == 'B' && data[2] == 'F' && data[3] == '0') pairs = gtxd_rbf(data);
                else pairs = gtxd_xml(std::string(data.begin(), data.end()));
                s.gtxd.push_back({file_local, std::move(pairs)});
            } else if (top_update && lname == "dlclist.xml" && e.path.size() > 0 && lower(e.path) == "common/data/dlclist.xml") {
                Bytes data = rpf.extract((int)i);
                s.dlclist.assign(data.begin(), data.end());
            } else if (lname_archive == "dlc.rpf" && lname == "setup2.xml" && e.parent == 0) {
                Bytes data = rpf.extract((int)i);
                s.setup.emplace_back(ai, setup_order(std::string(data.begin(), data.end())));
            }
        } catch (const std::exception& ex) {
            s.warnings.push_back(where + ": " + ex.what());
        }
    }
    s.archives[ai].file_count = count;
}

struct TopArchive {
    std::string rel;   // '/' separated from the game folder
    uint64_t bytes = 0;
};

std::vector<TopArchive> list_top_archives(const std::string& game) {
    std::vector<TopArchive> out;
    auto add = [&](const fs::path& p) {
        std::error_code ec;
        TopArchive t;
        t.rel = fs::relative(p, game, ec).generic_string();
        t.bytes = fs::file_size(p, ec);
        out.push_back(t);
    };
    std::vector<fs::path> base;
    for (auto& de : fs::directory_iterator(game)) {
        if (!de.is_regular_file()) continue;
        std::string n = lower(de.path().filename().string());
        if (ends_with(n, ".rpf") && (n == "common.rpf" || starts_with(n, "x64"))) base.push_back(de.path());
    }
    std::sort(base.begin(), base.end(), [](const fs::path& a, const fs::path& b) { return lower(a.filename().string()) < lower(b.filename().string()); });
    for (auto& p : base) add(p);
    // x64/audio/**.rpf (sound banks): archives like any other, base tier
    fs::path x64 = fs::path(game) / "x64";
    if (fs::exists(x64)) {
        std::vector<fs::path> audio;
        for (auto& de : fs::recursive_directory_iterator(x64))
            if (de.is_regular_file() && ends_with(lower(de.path().filename().string()), ".rpf")) audio.push_back(de.path());
        std::sort(audio.begin(), audio.end(), [](const fs::path& a, const fs::path& b) { return lower(a.generic_string()) < lower(b.generic_string()); });
        for (auto& p : audio) add(p);
    }
    for (const char* u : {"update.rpf", "update2.rpf"}) {
        fs::path p = fs::path(game) / "update" / u;
        if (fs::exists(p)) add(p);
    }
    fs::path dlc = fs::path(game) / "update" / "x64" / "dlcpacks";
    if (fs::exists(dlc)) {
        std::vector<fs::path> packs;
        for (auto& de : fs::recursive_directory_iterator(dlc)) {
            if (!de.is_regular_file()) continue;
            if (is_dlc_archive_name(lower(de.path().filename().string()))) packs.push_back(de.path());
        }
        std::sort(packs.begin(), packs.end(), [](const fs::path& a, const fs::path& b) { return lower(a.generic_string()) < lower(b.generic_string()); });
        for (auto& p : packs) add(p);
    }
    return out;
}

std::string read_exe_version(const std::string& game) {
    std::ifstream f(fs::path(game) / "versioninfo.txt");
    std::string line;
    while (std::getline(f, line)) {
        if (starts_with(line, "GTA5.exe")) {
            size_t a = line.find(' ');
            if (a == std::string::npos) break;
            size_t b = line.find(' ', a + 1);
            return line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        }
    }
    return {};
}

} // namespace

std::string LoadRank::label() const {
    switch (tier) {
        case -1: return "dlcpack " + dlc + " (not in dlclist.xml)";
        case 0: return "base";
        case 1: return "update.rpf";
        case 2: return "update2.rpf";
        default: return "dlcpack " + dlc + " (order " + std::to_string(order) + ", #" + std::to_string(index) + ")" + (patch ? " patched by update.rpf" : "");
    }
}

// ---------------------------------------------------------------------------------------
// Impl

struct GameIndex::Impl {
    GameIndexInfo info;
    std::vector<IndexedArchive> archives;
    Pool dirs;
    std::vector<LoadRank> ranks;
    std::vector<std::string> exts;
    std::vector<IndexedFile> files;
    std::vector<IndexedArchetype> archs;
    std::vector<uint32_t> name_hashes;
    Pool name_pool;
    std::vector<ParentRec> parents;
    std::unordered_map<std::string, uint16_t> ext_ids;

    mutable std::recursive_mutex mu;
    mutable std::unordered_map<uint32_t, std::shared_ptr<Rpf>> open_rpfs;
    mutable std::unordered_map<uint32_t, size_t> ytyp_counts;
    mutable bool counts_built = false;

    void finish_load() {
        ext_ids.clear();
        for (size_t i = 0; i < exts.size(); i++) ext_ids[exts[i]] = (uint16_t)i;
    }
    int ext_id(const std::string& ext) const {
        std::string e = lower(ext);
        if (!e.empty() && e[0] != '.') e = "." + e;
        auto it = ext_ids.find(e);
        return it == ext_ids.end() ? -1 : it->second;
    }
    bool active(uint16_t rank) const { return ranks[rank].tier >= 0; }

    std::shared_ptr<Rpf> open_archive(uint32_t i) const {
        std::lock_guard<std::recursive_mutex> g(mu);
        auto it = open_rpfs.find(i);
        if (it != open_rpfs.end()) return it->second;
        const IndexedArchive& a = archives.at(i);
        std::shared_ptr<Rpf> r;
        if (a.parent < 0) r = std::make_shared<Rpf>(Rpf::open((fs::path(info.game_dir) / a.path).string()));
        else r = std::make_shared<Rpf>(open_archive((uint32_t)a.parent)->open_child((int)a.parent_entry));
        open_rpfs[i] = r;
        return r;
    }

    /// [lo, hi) of the files with this name hash (and extension when ext >= 0).
    std::pair<size_t, size_t> file_range(uint32_t h, int ext) const {
        auto lo = std::lower_bound(files.begin(), files.end(), 0, [&](const IndexedFile& f, int) {
            return f.name_hash < h || (f.name_hash == h && ext >= 0 && (int)f.ext < ext);
        });
        auto hi = lo;
        while (hi != files.end() && hi->name_hash == h && (ext < 0 || (int)hi->ext == ext)) ++hi;
        return {(size_t)(lo - files.begin()), (size_t)(hi - files.begin())};
    }
    std::pair<size_t, size_t> arch_range(uint32_t h) const {
        auto lo = std::lower_bound(archs.begin(), archs.end(), h, [](const IndexedArchetype& a, uint32_t v) { return a.name < v; });
        auto hi = lo;
        while (hi != archs.end() && hi->name == h) ++hi;
        return {(size_t)(lo - archs.begin()), (size_t)(hi - archs.begin())};
    }
    int name_index(uint32_t h) const {
        auto it = std::lower_bound(name_hashes.begin(), name_hashes.end(), h);
        return it != name_hashes.end() && *it == h ? (int)(it - name_hashes.begin()) : -1;
    }

    // ----- cache -----
    static std::string default_cache_folder() { return (fs::path(home_dir()) / ".ragekit" / "index").string(); }

    void save(const std::string& path, uint64_t key, double scan_seconds) const {
        Writer w;
        w.raw("RKGI", 4);
        w.pod((uint32_t)GameIndex::kFormat);
        w.pod(key);
        w.pod(scan_seconds);
        w.str(info.game_dir);
        w.str(info.exe_version);
        w.pod((uint32_t)exts.size());
        for (auto& e : exts) w.str(e);
        w.pod((uint32_t)archives.size());
        for (auto& a : archives) { w.str(a.path); w.pod(a.parent); w.pod(a.parent_entry); w.pod(a.file_count); }
        w.pool(dirs);
        w.pod((uint32_t)ranks.size());
        for (auto& r : ranks) { w.pod(r.tier); w.pod(r.order); w.pod(r.index); w.pod(r.patch); w.str(r.dlc); }
        w.vec(files);
        w.vec(archs);
        w.vec(name_hashes);
        w.pool(name_pool);
        w.vec(parents);
        w.pod((uint32_t)info.warnings.size());
        for (auto& s : info.warnings) w.str(s);
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::string tmp = path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            f.write((const char*)w.b.data(), (std::streamsize)w.b.size());
            if (!f) throw Error("cannot write " + tmp);
        }
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) throw Error("cannot write " + path + ": " + ec.message());
    }

    bool load(const std::string& path, uint64_t key) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        std::streamsize n = f.tellg();
        if (n < 0) return false;
        f.seekg(0);
        Bytes buf((size_t)n);
        if (!f.read((char*)buf.data(), n)) return false;
        // read into locals and take them only once everything checks: a cache cut short or corrupted must leave the
        // index empty, or the scan that follows would add to what it left behind
        GameIndexInfo in;
        std::vector<std::string> xs;
        std::vector<IndexedArchive> as;
        Pool ds, np;
        std::vector<LoadRank> rs;
        std::vector<IndexedFile> fs;
        std::vector<IndexedArchetype> ar;
        std::vector<uint32_t> nh;
        std::vector<ParentRec> ps;
        try {
            CacheReader r{buf.data(), buf.data() + buf.size()};
            r.need(4);
            if (std::memcmp(r.p, "RKGI", 4) != 0) return false;
            r.p += 4;
            if (r.pod<uint32_t>() != GameIndex::kFormat) return false;
            if (r.pod<uint64_t>() != key) return false;
            in.scan_seconds = r.pod<double>();
            in.game_dir = r.str();
            in.exe_version = r.str();
            const uint32_t ne = r.pod<uint32_t>();
            r.need((size_t)ne * 4);   // every string costs at least its length word
            xs.resize(ne);
            for (auto& e : xs) e = r.str();
            const uint32_t na = r.pod<uint32_t>();
            r.need((size_t)na * 16);
            as.resize(na);
            for (auto& a : as) { a.path = r.str(); a.parent = r.pod<int32_t>(); a.parent_entry = r.pod<uint32_t>(); a.file_count = r.pod<uint32_t>(); }
            ds = r.pool();
            const uint32_t nr = r.pod<uint32_t>();
            r.need((size_t)nr * 20);
            rs.resize(nr);
            for (auto& k : rs) { k.tier = r.pod<int32_t>(); k.order = r.pod<int32_t>(); k.index = r.pod<int32_t>(); k.patch = r.pod<int32_t>(); k.dlc = r.str(); }
            fs = r.vec<IndexedFile>();
            ar = r.vec<IndexedArchetype>();
            nh = r.vec<uint32_t>();
            np = r.pool();
            ps = r.vec<ParentRec>();
            uint32_t nw = r.pod<uint32_t>();
            r.need((size_t)nw * 4);
            for (uint32_t i = 0; i < nw; i++) in.warnings.push_back(r.str());
            in.cache_bytes = (uint64_t)n;
        } catch (const std::exception&) {
            return false;
        }
        // the cross references: every index a row carries must land in its table
        if (np.size() != nh.size()) return false;
        for (size_t i = 1; i < ds.off.size(); i++) if (ds.off[i] < ds.off[i - 1]) return false;
        for (size_t i = 1; i < np.off.size(); i++) if (np.off[i] < np.off[i - 1]) return false;
        for (size_t i = 0; i < as.size(); i++)
            if (as[i].parent >= 0 && ((size_t)as[i].parent >= i)) return false;   // a parent archive is listed before its children
        for (const IndexedFile& f2 : fs)
            if (f2.archive >= as.size() || f2.dir >= ds.size() || f2.ext >= xs.size() || f2.rank >= rs.size() || f2.name_id >= np.size()) return false;
        for (const IndexedArchetype& a : ar) if (a.ytyp_file >= fs.size() || a.rank >= rs.size()) return false;
        if (!std::is_sorted(nh.begin(), nh.end())) return false;
        if (!std::is_sorted(ps.begin(), ps.end(), [](const ParentRec& a, const ParentRec& b) { return a.child < b.child; })) return false;
        info.scan_seconds = in.scan_seconds;
        info.game_dir = std::move(in.game_dir);
        info.exe_version = std::move(in.exe_version);
        info.warnings = std::move(in.warnings);
        info.cache_bytes = in.cache_bytes;
        exts = std::move(xs);
        archives = std::move(as);
        dirs = std::move(ds);
        ranks = std::move(rs);
        files = std::move(fs);
        archs = std::move(ar);
        name_hashes = std::move(nh);
        name_pool = std::move(np);
        parents = std::move(ps);
        finish_load();
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// open

std::string GameIndex::default_game_dir() { return default_game_dir(default_game()); }

std::string GameIndex::default_game_dir(Game g) {
    if (g == Game::Enhanced) {
        if (const char* e = std::getenv("RAGEKIT_GAME_ENHANCED")) if (*e && fs::exists(e)) return e;
        for (const char* c : {"C:/Program Files/Rockstar Games/Grand Theft Auto V Enhanced", "C:/Program Files (x86)/Steam/steamapps/common/Grand Theft Auto V Enhanced",
                              "C:/Program Files/Epic Games/GTAVEnhanced"})
            if (fs::exists(fs::path(c) / "GTA5_Enhanced.exe")) return c;
        return {};
    }
    if (const char* e = std::getenv("RAGEKIT_GAME")) if (*e && fs::exists(e)) return e;
    for (const char* c : {"C:/Program Files/Rockstar Games/Grand Theft Auto V Legacy", "C:/Program Files/Rockstar Games/Grand Theft Auto V",
                          "C:/Program Files (x86)/Steam/steamapps/common/Grand Theft Auto V Legacy", "C:/Program Files (x86)/Steam/steamapps/common/Grand Theft Auto V",
                          "C:/Program Files/Epic Games/GTAV"})
        if (fs::exists(fs::path(c) / "GTA5.exe")) return c;
    return {};
}

Game GameIndex::game_of_dir(const std::string& game_dir) {
    return fs::exists(fs::path(game_dir) / "GTA5_Enhanced.exe") && !fs::exists(fs::path(game_dir) / "GTA5.exe") ? Game::Enhanced : Game::Legacy;
}

GameIndex GameIndex::open(const std::string& game_dir_in, const GameIndexOptions& opt) {
    auto t0 = std::chrono::steady_clock::now();
    auto say = [&](const std::string& s) { if (opt.progress) opt.progress(s); };
    std::string game = fs::path(game_dir_in).generic_string();
    while (game.size() > 1 && game.back() == '/') game.pop_back();
    if (!fs::exists(fs::path(game) / "GTA5.exe") && !fs::exists(fs::path(game) / "GTA5_Enhanced.exe") && !fs::exists(fs::path(game) / "x64a.rpf"))
        throw Error("'" + game + "' does not look like a GTA V install (no GTA5.exe / GTA5_Enhanced.exe / x64a.rpf)");

    GameIndex gi;
    gi.d_ = std::make_shared<Impl>();
    Impl& d = *gi.d_;
    d.info.game_dir = game;
    d.info.game = game_of_dir(game);
    d.info.exe_version = read_exe_version(game);

    // cache key: format, folder, exe, every archive's size and time
    std::vector<TopArchive> tops = list_top_archives(game);
    uint64_t key = 1469598103934665603ull;
    key = fnv(key, "format" + std::to_string(kFormat));
    key = fnv(key, lower(game));
    key = fnv(key, d.info.exe_version);
    auto stat_str = [&](const fs::path& p) {
        std::error_code ec;
        auto sz = fs::file_size(p, ec);
        auto tm = fs::last_write_time(p, ec).time_since_epoch().count();
        return std::to_string(sz) + ":" + std::to_string((long long)tm);
    };
    key = fnv(key, stat_str(fs::path(game) / (d.info.game == Game::Enhanced ? "GTA5_Enhanced.exe" : "GTA5.exe")));
    for (auto& t : tops) key = fnv(fnv(key, t.rel), stat_str(fs::path(game) / t.rel));
    char keybuf[32];
    std::snprintf(keybuf, sizeof keybuf, "%016llx", (unsigned long long)key);
    d.info.key = keybuf;
    std::string cache_folder = opt.cache_folder.empty() ? Impl::default_cache_folder() : opt.cache_folder;
    d.info.cache_path = (fs::path(cache_folder) / (d.info.key + ".bin")).string();

    if (opt.use_cache && !opt.rebuild && d.load(d.info.cache_path, key)) {
        d.info.from_cache = true;
        d.info.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return gi;
    }

    // ---- scan every top level archive on a few threads ----
    auto keys = RpfKeys::default_keys();
    if (!keys->has_aes() && !keys->has_ng()) throw Error("no RPF keys in " + RpfKeys::default_folder() + " (run `rk rpf keys <GTA5.exe>`)");
    size_t nthreads = opt.threads > 0 ? (size_t)opt.threads : std::min<size_t>(8, std::max<unsigned>(2, std::thread::hardware_concurrency()));
    std::vector<size_t> by_size(tops.size());
    for (size_t i = 0; i < by_size.size(); i++) by_size[i] = i;
    std::sort(by_size.begin(), by_size.end(), [&](size_t a, size_t b) { return tops[a].bytes > tops[b].bytes; });
    std::vector<Scan> scans(tops.size());
    std::atomic<size_t> next{0}, done{0};
    std::mutex say_mu;
    std::vector<std::thread> pool;
    for (size_t t = 0; t < nthreads; t++)
        pool.emplace_back([&] {
            for (;;) {
                size_t k = next.fetch_add(1);
                if (k >= by_size.size()) return;
                size_t ti = by_size[k];
                Scan& s = scans[ti];
                try {
                    Rpf rpf = Rpf::open((fs::path(game) / tops[ti].rel).string(), keys);
                    scan_archive(s, rpf, tops[ti].rel, -1, 0, lower(tops[ti].rel) == "update/update.rpf");
                } catch (const std::exception& ex) {
                    s.warnings.push_back(tops[ti].rel + ": " + ex.what());
                }
                size_t n = ++done;
                std::lock_guard<std::mutex> g(say_mu);
                say("[" + std::to_string(n) + "/" + std::to_string(tops.size()) + "] " + tops[ti].rel + ": " + std::to_string(s.files.size()) + " files, " +
                    std::to_string(s.archs.size()) + " archetypes");
            }
        });
    for (auto& th : pool) th.join();

    // ---- merge in a fixed order ----
    std::vector<std::string> dir_paths;       // full path (archive/dir), lower case, for ranking
    std::vector<IndexedFile> files;
    std::vector<IndexedArchetype> archs;
    std::unordered_map<std::string, uint32_t> gids; // stems of all scans -> provisional id
    std::vector<std::string> gstems;
    auto stem_id = [&](const std::string& st) {
        auto it = gids.try_emplace(st, (uint32_t)gstems.size());
        if (it.second) gstems.push_back(st);
        return it.first->second;
    };
    std::string dlclist;
    std::vector<std::pair<uint32_t, int>> setup_orders; // global archive, order
    struct G { uint32_t file; TxdPairs pairs; };
    std::vector<G> gtxds;
    std::vector<std::string> warnings;
    Pool dirs;
    std::unordered_map<std::string, uint16_t> ext_ids;
    std::vector<std::string> exts;
    for (Scan& s : scans) {
        uint32_t ab = (uint32_t)d.archives.size(), db = (uint32_t)dirs.size(), fb = (uint32_t)files.size();
        for (auto& a : s.archives) { IndexedArchive c = a; if (c.parent >= 0) c.parent += (int32_t)ab; d.archives.push_back(c); }
        for (size_t i = 0; i < s.dirs.size(); i++) {
            dirs.add(s.dirs[i]);
            dir_paths.push_back(lower(d.archives[ab + s.dir_archive[i]].path + (s.dirs[i].empty() ? "" : "/" + s.dirs[i])));
        }
        std::vector<uint16_t> ext_map(s.exts.size());
        for (size_t i = 0; i < s.exts.size(); i++) {
            auto it = ext_ids.find(s.exts[i]);
            if (it == ext_ids.end()) { it = ext_ids.emplace(s.exts[i], (uint16_t)exts.size()).first; exts.push_back(s.exts[i]); }
            ext_map[i] = it->second;
        }
        std::vector<uint32_t> stem_map(s.stems.size());
        for (size_t i = 0; i < s.stems.size(); i++) stem_map[i] = stem_id(s.stems[i]);
        for (IndexedFile f : s.files) {
            f.archive += ab; f.dir += db; f.ext = ext_map[f.ext]; f.name_id = stem_map[f.name_id];
            files.push_back(f);
        }
        for (IndexedArchetype a : s.archs) { a.ytyp_file += fb; archs.push_back(a); }
        if (!s.dlclist.empty()) dlclist = s.dlclist;
        for (auto& so : s.setup) setup_orders.emplace_back(so.first + ab, so.second);
        for (auto& g : s.gtxd) gtxds.push_back({g.file + fb, std::move(g.pairs)});
        for (auto& w : s.warnings) warnings.push_back(w);
    }
    // ---- load order ----
    Packs packs;
    std::unordered_map<uint32_t, int> order_of;
    for (auto& so : setup_orders) order_of[so.first] = so.second;
    for (size_t i = 0; i < d.archives.size(); i++) {
        std::string lp = lower(d.archives[i].path);
        size_t cut = lp.rfind('/');
        std::string lname = cut == std::string::npos ? lp : lp.substr(cut + 1);
        if (lname != "dlc.rpf" || starts_with(lp, "mods/")) continue;
        Pack p;
        p.prefix = lp.substr(0, lp.size() - 7);
        std::string trimmed = p.prefix.substr(0, p.prefix.size() - 1);
        p.name = trimmed.substr(trimmed.rfind('/') == std::string::npos ? 0 : trimmed.rfind('/') + 1);
        auto oit = order_of.find((uint32_t)i);
        p.order = oit != order_of.end() ? oit->second : 0;
        packs.by_prefix[p.prefix] = packs.all.size();
        packs.by_name[p.name] = packs.all.size();
        packs.all.push_back(p);
    }
    if (dlclist.empty()) warnings.push_back("update/update.rpf: common/data/dlclist.xml not found; every dlc pack treated as unmounted");
    int list_index = 0;
    for (const std::string& item : xml_items(dlclist)) {
        std::string t = item;
        std::replace(t.begin(), t.end(), '\\', '/');
        while (!t.empty() && t.back() == '/') t.pop_back();
        std::string name = lower(t.substr(t.rfind('/') == std::string::npos ? 0 : t.rfind('/') + 1));
        if (name.empty()) continue;
        auto it = packs.by_name.find(name);
        if (it == packs.by_name.end()) { warnings.push_back("dlclist.xml names " + name + " but there is no dlcpacks/" + name + "/dlc.rpf"); list_index++; continue; }
        packs.all[it->second].list_index = list_index++;
    }
    {
        std::vector<size_t> mounted;
        for (size_t i = 0; i < packs.all.size(); i++) if (packs.all[i].listed()) mounted.push_back(i);
        std::sort(mounted.begin(), mounted.end(), [&](size_t a, size_t b) {
            const Pack &x = packs.all[a], &y = packs.all[b];
            return x.order != y.order ? x.order < y.order : x.list_index < y.list_index;
        });
        for (size_t i = 0; i < mounted.size(); i++) packs.all[mounted[i]].mount = (int)i;
    }
    // rank per directory, then dense rank ids
    std::vector<LoadRank> dir_rank(dirs.size());
    for (size_t i = 0; i < dirs.size(); i++) dir_rank[i] = rank_of(dir_paths[i] + "/", packs); // trailing slash: the folder "x64w.rpf/dlcpacks/mpbeach" is the pack too
    {
        std::vector<LoadRank> uniq;
        for (auto& r : dir_rank) uniq.push_back(r);
        std::sort(uniq.begin(), uniq.end(), rank_less);
        uniq.erase(std::unique(uniq.begin(), uniq.end(), rank_eq), uniq.end());
        d.ranks = uniq;
    }
    auto rank_id = [&](const LoadRank& r) -> uint16_t {
        auto it = std::lower_bound(d.ranks.begin(), d.ranks.end(), r, rank_less);
        return (uint16_t)(it - d.ranks.begin());
    };
    std::vector<uint16_t> dir_rank_id(dirs.size());
    for (size_t i = 0; i < dirs.size(); i++) dir_rank_id[i] = rank_id(dir_rank[i]);
    for (auto& f : files) f.rank = dir_rank_id[f.dir];
    for (auto& a : archs) a.rank = files[a.ytyp_file].rank;

    // txd parents: the highest ranked file naming a child wins
    {
        std::vector<size_t> order(gtxds.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return files[gtxds[a].file].rank > files[gtxds[b].file].rank; });
        std::unordered_map<uint32_t, uint32_t> seen;
        for (size_t oi : order)
            for (auto& pr : gtxds[oi].pairs) {
                uint32_t c = joaat_lower(pr.first), p = joaat_lower(pr.second);
                seen.try_emplace(c, p);
                stem_id(lower(pr.first));
                stem_id(lower(pr.second));
            }
        for (auto& kv : seen) d.parents.push_back({kv.first, kv.second});
        std::sort(d.parents.begin(), d.parents.end(), [](const ParentRec& a, const ParentRec& b) { return a.child < b.child; });
    }

    // sort files, remap the archetypes' file references
    std::vector<uint32_t> perm(files.size());
    for (size_t i = 0; i < perm.size(); i++) perm[i] = (uint32_t)i;
    std::sort(perm.begin(), perm.end(), [&](uint32_t a, uint32_t b) {
        const IndexedFile &x = files[a], &y = files[b];
        if (x.name_hash != y.name_hash) return x.name_hash < y.name_hash;
        if (x.ext != y.ext) return x.ext < y.ext;
        if (x.rank != y.rank) return x.rank < y.rank;
        return a < b; // scan order
    });
    std::vector<uint32_t> newpos(files.size());
    d.files.resize(files.size());
    for (size_t i = 0; i < perm.size(); i++) { d.files[i] = files[perm[i]]; newpos[perm[i]] = (uint32_t)i; }
    for (auto& a : archs) a.ytyp_file = newpos[a.ytyp_file];
    std::stable_sort(archs.begin(), archs.end(), [](const IndexedArchetype& a, const IndexedArchetype& b) {
        return a.name != b.name ? a.name < b.name : a.rank < b.rank;
    });
    d.archs = std::move(archs);
    d.dirs = std::move(dirs);
    d.exts = std::move(exts);

    // names sorted by (hash, string): two stems with the same hash both keep a row, and a file points at its own
    {
        std::vector<std::pair<uint32_t, uint32_t>> v; // hash, provisional id
        v.reserve(gstems.size());
        for (size_t i = 0; i < gstems.size(); i++) v.emplace_back(joaat_lower(gstems[i]), (uint32_t)i);
        std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : gstems[a.second] < gstems[b.second]; });
        std::vector<uint32_t> final_id(gstems.size());
        for (size_t i = 0; i < v.size(); i++) { final_id[v[i].second] = (uint32_t)i; d.name_hashes.push_back(v[i].first); d.name_pool.add(gstems[v[i].second]); }
        for (auto& f : d.files) f.name_id = final_id[f.name_id];
    }
    std::sort(warnings.begin(), warnings.end());
    d.info.warnings = warnings;
    d.finish_load();
    d.info.scan_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (opt.use_cache) {
        try {
            d.save(d.info.cache_path, key, d.info.scan_seconds);
            std::error_code ec;
            d.info.cache_bytes = fs::file_size(d.info.cache_path, ec);
        } catch (const std::exception& ex) {
            d.info.warnings.push_back(std::string("cache not written: ") + ex.what());
        }
    }
    d.info.from_cache = false;
    d.info.seconds = d.info.scan_seconds;
    return gi;
}

// ---------------------------------------------------------------------------------------
// Queries

const GameIndexInfo& GameIndex::info() const { return d_->info; }
const std::vector<IndexedArchive>& GameIndex::archives() const { return d_->archives; }
const std::vector<IndexedFile>& GameIndex::files() const { return d_->files; }
const std::vector<IndexedArchetype>& GameIndex::archetypes() const { return d_->archs; }
const std::vector<std::string>& GameIndex::extensions() const { return d_->exts; }
const std::vector<LoadRank>& GameIndex::ranks() const { return d_->ranks; }

bool GameIndex::known_name(uint32_t hash, std::string* out) const {
    int i = d_->name_index(hash);
    if (i < 0) return false;
    if (out) *out = std::string(d_->name_pool.get((size_t)i));
    return true;
}
std::string GameIndex::name(uint32_t hash) const {
    std::string s;
    return known_name(hash, &s) ? s : hash_name(hash);
}

std::string GameIndex::file_name(const IndexedFile& f) const { return std::string(d_->name_pool.get(f.name_id)) + d_->exts.at(f.ext); }
const std::string& GameIndex::extension(const IndexedFile& f) const { return d_->exts.at(f.ext); }
const LoadRank& GameIndex::rank(const IndexedFile& f) const { return d_->ranks.at(f.rank); }
std::string GameIndex::archive_path(const IndexedFile& f) const { return d_->archives.at(f.archive).path; }
std::string GameIndex::path(const IndexedFile& f) const {
    std::string dp(d_->dirs.get(f.dir));
    return d_->archives.at(f.archive).path + (dp.empty() ? "" : "/" + dp) + "/" + file_name(f);
}

std::vector<IndexedFile> GameIndex::versions(uint32_t h, const std::string& ext) const {
    int e = d_->ext_id(ext);
    if (e < 0) return {};
    auto [lo, hi] = d_->file_range(h, e);
    return std::vector<IndexedFile>(d_->files.begin() + (std::ptrdiff_t)lo, d_->files.begin() + (std::ptrdiff_t)hi);
}
std::optional<IndexedFile> GameIndex::resolve_file(uint32_t h, const std::string& ext) const {
    int e = d_->ext_id(ext);
    if (e < 0) return std::nullopt;
    auto [lo, hi] = d_->file_range(h, e);
    if (lo == hi) return std::nullopt;
    const IndexedFile& f = d_->files[hi - 1];
    if (!d_->active(f.rank)) return std::nullopt;
    return f;
}
std::optional<IndexedFile> GameIndex::resolve_file(const std::string& name, const std::string& ext) const {
    return resolve_file(joaat_lower(name), ext);
}
std::vector<IndexedFile> GameIndex::resolve_stem(uint32_t h) const {
    std::vector<IndexedFile> out;
    auto [lo, hi] = d_->file_range(h, -1);
    for (size_t i = lo; i < hi; i++) {
        bool last = i + 1 == hi || d_->files[i + 1].ext != d_->files[i].ext;
        if (last && d_->active(d_->files[i].rank)) out.push_back(d_->files[i]);
    }
    return out;
}

std::vector<IndexedFile> GameIndex::find_files(const std::string& substring, const std::string& ext, size_t limit) const {
    std::string needle = lower(substring);
    int e = ext.empty() ? -1 : d_->ext_id(ext);
    if (!ext.empty() && e < 0) return {};
    std::vector<std::pair<std::string, uint32_t>> hits;
    for (size_t i = 0; i < d_->name_pool.size(); i++) {
        std::string_view s = d_->name_pool.get(i);
        if (s.find(needle) != std::string_view::npos) hits.emplace_back(std::string(s), d_->name_hashes[i]);
    }
    std::sort(hits.begin(), hits.end());
    std::vector<IndexedFile> out;
    std::unordered_set<uint32_t> seen;
    for (auto& h : hits) {
        if (!seen.insert(h.second).second) continue;
        for (auto& f : resolve_stem(h.second)) {
            if (e >= 0 && f.ext != e) continue;
            out.push_back(f);
            if (limit && out.size() >= limit) return out;
        }
    }
    return out;
}

ResolvedDrawable GameIndex::resolve_drawable(uint32_t h) const {
    ResolvedDrawable r;
    if (auto f = resolve_file(h, ".ydr")) { r.type = ResolvedDrawable::Ydr; r.file = f; r.drawable_name = h; return r; }
    if (auto f = resolve_file(h, ".yft")) { r.type = ResolvedDrawable::Yft; r.file = f; r.drawable_name = h; return r; }
    return r;
}
ResolvedDrawable GameIndex::resolve_drawable(const std::string& name) const { return resolve_drawable(hash_or_name(name)); }
ResolvedDrawable GameIndex::resolve_drawable(const IndexedArchetype& a) const {
    // the model is named by the asset name (the archetype name when there is none), as in CodeWalker
    uint32_t h = a.asset_name ? a.asset_name : a.name;
    if (a.drawable_dictionary) {
        if (auto f = resolve_file(a.drawable_dictionary, ".ydd")) {
            ResolvedDrawable r;
            r.type = ResolvedDrawable::Ydd; r.file = f; r.drawable_name = h;
            return r;
        }
    }
    ResolvedDrawable r = resolve_drawable(h);
    if (r.type == ResolvedDrawable::None && h != a.name) r = resolve_drawable(a.name);
    return r;
}
std::optional<IndexedFile> GameIndex::resolve_txd(uint32_t h) const { return resolve_file(h, ".ytd"); }
std::optional<IndexedFile> GameIndex::resolve_txd(const std::string& name) const { return resolve_txd(hash_or_name(name)); }

std::vector<uint32_t> GameIndex::txd_parents(uint32_t txd) const {
    std::vector<uint32_t> out;
    uint32_t cur = txd;
    for (int i = 0; i < 16; i++) {
        auto it = std::lower_bound(d_->parents.begin(), d_->parents.end(), cur, [](const ParentRec& a, uint32_t v) { return a.child < v; });
        if (it == d_->parents.end() || it->child != cur) break;
        cur = it->parent;
        if (cur == txd || std::find(out.begin(), out.end(), cur) != out.end()) break;
        out.push_back(cur);
    }
    return out;
}

std::optional<IndexedArchetype> GameIndex::archetype(uint32_t h) const {
    auto [lo, hi] = d_->arch_range(h);
    if (lo == hi) return std::nullopt;
    const IndexedArchetype& a = d_->archs[hi - 1];
    if (!d_->active(a.rank)) return std::nullopt;
    return a;
}
std::optional<IndexedArchetype> GameIndex::archetype(const std::string& name) const { return archetype(hash_or_name(name)); }
std::vector<IndexedArchetype> GameIndex::archetype_versions(uint32_t h) const {
    auto [lo, hi] = d_->arch_range(h);
    return std::vector<IndexedArchetype>(d_->archs.begin() + (std::ptrdiff_t)lo, d_->archs.begin() + (std::ptrdiff_t)hi);
}
std::vector<IndexedArchetype> GameIndex::find_archetypes(const std::string& substring, size_t limit) const {
    std::string needle = lower(substring);
    std::vector<std::pair<std::string, size_t>> hits;
    const auto& A = d_->archs;
    for (size_t i = 0; i < A.size();) {
        size_t j = i;
        while (j < A.size() && A[j].name == A[i].name) ++j;
        const IndexedArchetype& w = A[j - 1];
        if (d_->active(w.rank)) {
            std::string n = name(w.name);
            if (n.find(needle) != std::string::npos) hits.emplace_back(n, j - 1);
        }
        i = j;
    }
    std::sort(hits.begin(), hits.end());
    std::vector<IndexedArchetype> out;
    for (auto& h : hits) {
        out.push_back(A[h.second]);
        if (limit && out.size() >= limit) break;
    }
    return out;
}
const IndexedFile& GameIndex::ytyp_of(const IndexedArchetype& a) const { return d_->files.at(a.ytyp_file); }
size_t GameIndex::archetype_count(uint32_t ytyp_file) const {
    std::lock_guard<std::recursive_mutex> g(d_->mu);
    if (!d_->counts_built) {
        for (auto& a : d_->archs) d_->ytyp_counts[a.ytyp_file]++;
        d_->counts_built = true;
    }
    auto it = d_->ytyp_counts.find(ytyp_file);
    return it == d_->ytyp_counts.end() ? 0 : it->second;
}

Bytes GameIndex::extract(const IndexedFile& f) const {
    auto rpf = d_->open_archive(f.archive);
    const RpfEntry& e = rpf->entries().at(f.entry);
    if (e.is_archive()) return rpf->extract_raw((int)f.entry);
    return rpf->extract((int)f.entry);
}

} // namespace rk
