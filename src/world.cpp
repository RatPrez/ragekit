// ragekit - the game world index. See include/ragekit/world.h.
//
// Cache file (<GameIndex cache folder>/world_<game index key>.bin), format WorldIndex::kFormat, little endian:
//   "RKWI" u32 format | str game index key | f64 scan_seconds
//   maps: u32 n, n * WorldMap | mlo instances: u32 n, n * WorldMloInstance | bounds: u32 n, n * WorldBounds
//   vec<str> warnings
// "str" = u32 length + bytes.
//
// The bounds store of a `*_cache_y.dat`: after the line "BoundsStore\n" a u32 byte count, then 32-byte items
// { u32 name, vec3 min, vec3 max, u32 layer } (CodeWalker CacheDatFile).
#include "ragekit/world.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include "ragekit/bounds.h"
#include "ragekit/error.h"
#include "ragekit/hash.h"
#include "ragekit/manifest.h"
#include "ragekit/xml_dom.h"

namespace fs = std::filesystem;

namespace rk {

static_assert(std::is_trivially_copyable_v<WorldMap>);
static_assert(std::is_trivially_copyable_v<WorldMloInstance>);
static_assert(std::is_trivially_copyable_v<WorldBounds>);

// ---------------------------------------------------------------------------------------- maths

Quat entity_orientation(const Entity& e) {
    const Vec4& r = e.rotation;
    if (e.is_mlo_instance) return {r.x, r.y, r.z, r.w};
    return {-r.x, -r.y, -r.z, r.w};
}

Vec3 rotate(const Quat& q, const Vec3& v) {
    const Vec3 u{q.x, q.y, q.z};
    auto cross = [](const Vec3& a, const Vec3& b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
    const Vec3 t = cross(u, v) * 2.f;
    return v + t * q.w + cross(u, t);
}

void transform_box(const Vec3& mn, const Vec3& mx, const Vec3& scale, const Quat& q, const Vec3& pos, Vec3& out_min, Vec3& out_max) {
    out_min = {INFINITY, INFINITY, INFINITY};
    out_max = {-INFINITY, -INFINITY, -INFINITY};
    for (int i = 0; i < 8; i++) {
        const Vec3 c{(i & 1 ? mx.x : mn.x) * scale.x, (i & 2 ? mx.y : mn.y) * scale.y, (i & 4 ? mx.z : mn.z) * scale.z};
        const Vec3 w = rotate(q, c) + pos;
        out_min = vmin(out_min, w);
        out_max = vmax(out_max, w);
    }
}

namespace {

bool overlaps(const Vec3& amin, const Vec3& amax, const Vec3& bmin, const Vec3& bmax) {
    return amin.x <= bmax.x && amax.x >= bmin.x && amin.y <= bmax.y && amax.y >= bmin.y && amin.z <= bmax.z && amax.z >= bmin.z;
}

std::string_view trim_view(std::string_view s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.remove_suffix(1);
    return s;
}

bool ends_with(const std::string& s, const char* e) {
    const size_t n = std::strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}

std::string home_dir() {
    const char* h = std::getenv("USERPROFILE");
    if (!h || !*h) h = std::getenv("HOME");
    return h ? h : ".";
}

struct Writer {
    Bytes b;
    void raw(const void* p, size_t n) { const auto* c = (const uint8_t*)p; b.insert(b.end(), c, c + n); }
    template <class T> void pod(const T& v) { raw(&v, sizeof(T)); }
    void str(const std::string& s) { pod((uint32_t)s.size()); raw(s.data(), s.size()); }
    template <class T> void vec(const std::vector<T>& v) { pod((uint32_t)v.size()); raw(v.data(), v.size() * sizeof(T)); }
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    void need(size_t n) const { if ((size_t)(end - p) < n) throw Error("world index cache is truncated"); }
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
};

int ext_id(const GameIndex& gi, const char* ext) {
    const auto& e = gi.extensions();
    for (size_t i = 0; i < e.size(); i++) if (e[i] == ext) return (int)i;
    return -1;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/// A data file path of content.xml as a key: lower case, '/' separated ("dlcmpheist:/%platform%/levels/gta5/x.rpf").
std::string norm_path(std::string s) {
    s = lower(std::string(trim_view(s)));
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

/// Which archives the game mounts for the map (see world.h). Archives are named by their logical path: the device
/// path a content.xml uses ("platform:/levels/..." for the base game and the title update's copies,
/// "<device>:/%platform%/..." for a pack and its dlc_patch copy); archives outside the map have none.
struct MountModel {
    std::vector<std::string> logical;          // per GameIndex archive
    std::vector<int8_t> active;                // per GameIndex archive: 1 mounted
    std::unordered_set<uint32_t> disabled;     // names of single files a change set invalidated (".interior" entries)
    uint32_t invalidated = 0, enabled = 0;

    static std::string platformize(const std::string& tail) {
        return tail.compare(0, 4, "x64/") == 0 ? "%platform%/" + tail.substr(4) : tail;
    }

    void build(const GameIndex& gi, bool sp_map, std::vector<std::string>& warnings) {
        // the mounted packs in load order: device name, content.xml, setup2.xml (the newest copy, dlc_patch included)
        struct Pack { std::string name, device; int rank = 0; XmlElement content, setup; bool has_content = false, has_setup = false; };
        std::vector<Pack> packs;
        std::unordered_map<std::string, size_t> by_name;
        std::unordered_map<std::string, int> content_rank, setup_rank;
        const int xml = ext_id(gi, ".xml");
        for (const IndexedFile& f : gi.files()) {
            if ((int)f.ext != xml) continue;
            const LoadRank& r = gi.rank(f);
            if (r.tier != 3) continue;
            const std::string fn = gi.file_name(f);
            const bool is_content = fn == "content.xml", is_setup = fn == "setup2.xml";
            if (!is_content && !is_setup) continue;
            const std::string dir = lower(gi.path(f));   // only the pack's own root copy: "<pack>/dlc.rpf/content.xml" or "dlc_patch/<pack>/content.xml"
            const std::string pack = lower(r.dlc);
            const bool root = dir.find("/dlcpacks/" + pack + "/dlc.rpf/" + fn) != std::string::npos || dir.find("/dlc_patch/" + pack + "/" + fn) != std::string::npos;
            if (!root) continue;
            auto [it, fresh] = by_name.emplace(pack, packs.size());
            if (fresh) { packs.push_back({}); packs.back().name = pack; }
            Pack& p = packs[it->second];
            p.rank = std::max(p.rank, (int)f.rank);
            auto& best = is_content ? content_rank : setup_rank;
            auto bi = best.find(pack);
            if (bi != best.end() && bi->second > (int)f.rank) continue;
            best[pack] = (int)f.rank;
            try {
                const Bytes b = gi.extract(f);
                XmlElement e = parse_xml(std::string_view((const char*)b.data(), b.size()));
                if (is_content) { p.content = std::move(e); p.has_content = true; }
                else { p.setup = std::move(e); p.has_setup = true; p.device = lower(p.setup.child_text("deviceName")); }
            } catch (const std::exception& ex) {
                warnings.push_back(gi.path(f) + ": " + ex.what());
            }
        }
        std::sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) { return a.rank < b.rank; });
        std::unordered_map<std::string, std::string> device_of;
        for (auto& p : packs) if (!p.device.empty()) device_of[p.name] = p.device;

        // logical path of every archive
        const auto& archives = gi.archives();
        logical.assign(archives.size(), std::string());
        for (size_t i = 0; i < archives.size(); i++) {
            const std::string lp = lower(archives[i].path);
            static const std::string kPatch = "update/update.rpf/dlc_patch/", kPacks = "dlcpacks/";
            const size_t packs_at = lp.find(kPacks);   // update/x64/dlcpacks/<pack>/... or x64w.rpf/dlcpacks/<pack>/...
            std::string out;
            if (lp.compare(0, kPatch.size(), kPatch) == 0 || (packs_at != std::string::npos && (packs_at == 0 || lp[packs_at - 1] == '/'))) {
                const bool patch = lp.compare(0, kPatch.size(), kPatch) == 0;
                std::string rest = lp.substr(patch ? kPatch.size() : packs_at + kPacks.size());
                const size_t s1 = rest.find('/');
                const std::string pack = rest.substr(0, s1);
                auto d = device_of.find(pack);
                if (d != device_of.end() && s1 != std::string::npos) {
                    std::string tail = rest.substr(s1 + 1);
                    if (!patch) {   // skip "dlc.rpf" / "dlc1.rpf": every archive of a pack shares its device
                        const size_t s2 = tail.find('/');
                        tail = s2 == std::string::npos ? std::string() : tail.substr(s2 + 1);
                    }
                    out = d->second + ":/" + platformize(tail);
                }
            } else if (lp.compare(0, 18, "update/update.rpf/") == 0 || lp.compare(0, 19, "update/update2.rpf/") == 0) {
                const std::string tail = lp.substr(lp.find(".rpf/") + 5);
                if (tail.compare(0, 4, "x64/") == 0) out = "platform:/" + tail.substr(4);
            } else if (lp.size() > 9 && lp[8] == '/' && lp.compare(0, 3, "x64") == 0 && lp.compare(4, 4, ".rpf") == 0) {
                out = "platform:/" + lp.substr(9);   // x64a.rpf .. x64w.rpf
            }
            logical[i] = out;
        }

        // states: a pack's data files start as their `disabled` flag says; change sets turn them on and off. A pack
        // archive "<device>:/%platform%/X" overlays the base "platform:/X": turning the base path off later turns off
        // every overlay registered so far (CodeWalker's overlay rule: patchday1ng's beverly_metadata.rpf goes when
        // mpheist invalidates platform:/.../beverly_metadata.rpf and enables its own)
        std::unordered_map<std::string, bool> state;
        std::unordered_map<std::string, std::vector<std::string>> overlays;
        auto is_rpf = [](const std::string& f) { return f.size() > 4 && f.compare(f.size() - 4, 4, ".rpf") == 0; };
        auto overlay_key = [](const std::string& f) -> std::string {
            const size_t c = f.find(":/%platform%/");
            return c == std::string::npos || f.compare(0, 9, "platform:") == 0 ? std::string() : "platform:/" + f.substr(c + 13);
        };
        auto set_rpf = [&](const std::string& f, bool on) {
            auto it = state.find(f);
            const bool was = it == state.end() ? f.compare(0, 10, "platform:/") == 0 : it->second;
            if (was != on) (on ? enabled : invalidated)++;
            state[f] = on;
            if (on) {
                const std::string k = overlay_key(f);
                if (!k.empty()) overlays[k].push_back(f);
            } else if (f.compare(0, 10, "platform:/") == 0) {
                auto ov = overlays.find(f);
                if (ov != overlays.end()) {
                    for (const std::string& o : ov->second) { if (state[o]) invalidated++; state[o] = false; }
                    overlays.erase(ov);
                }
            }
        };
        for (auto& p : packs) {
            if (!p.has_content) continue;
            if (const XmlElement* df = p.content.child("dataFiles"))
                for (const XmlElement* it : df->children_named("Item")) {
                    const std::string fn = norm_path(it->child_text("filename"));
                    if (!is_rpf(fn)) continue;
                    if (lower(it->child_value("disabled", "false")) == "true") state.emplace(fn, false);
                    else set_rpf(fn, true);
                }
        }
        auto set = [&](const std::string& raw, bool on) {
            const std::string f = norm_path(raw);
            if (f.empty()) return;
            if (is_rpf(f)) { set_rpf(f, on); return; }
            std::string stem = f.substr(f.rfind('/') == std::string::npos ? 0 : f.rfind('/') + 1);   // a single file: "x_milo_.interior"
            stem = stem.substr(stem.find(':') == std::string::npos ? 0 : stem.find(':') + 1);
            stem = stem.substr(0, stem.rfind('.'));
            if (on) disabled.erase(joaat_lower(stem));
            else disabled.insert(joaat_lower(stem));
        };
        auto apply = [&](const XmlElement& cs) {
            auto items = [](const XmlElement* list) { return list ? list->children_named("Item") : std::vector<const XmlElement*>{}; };
            for (auto* f : items(cs.child("filesToEnable"))) set(f->text, true);
            for (auto* f : items(cs.child("filesToDisable"))) set(f->text, false);
            if (const XmlElement* mc = cs.child("mapChangeSetData"))
                for (const XmlElement* m : mc->children_named("Item")) {
                    for (auto* f : items(m->child("filesToInvalidate"))) set(f->text, false);
                    for (auto* f : items(m->child("filesToDisable"))) set(f->text, false);
                    for (auto* f : items(m->child("filesToEnable"))) set(f->text, true);
                }
        };
        // group by group, each over the packs in load order: what the game runs at startup (the title update's
        // groups, the packs' startup sets), then GROUP_MAP, the multiplayer map changes (skipped for the SP map)
        static const char* kGroups[] = {"GROUP_EARLY_ON", "GROUP_TITLEUPDATE_STARTUP", "GROUP_STARTUP", "GROUP_UPDATE_STREAMING",
                                        "GROUP_UPDATE_DLC_PATCH", "GROUP_UPDATE_DLC_METADATA", "GROUP_UPDATE_WEAPON_PATCH",
                                        "GROUP_POST_DLC_PATCH", "GROUP_MAP"};
        for (const char* g : kGroups)
            for (auto& p : packs) {
                if (!p.has_setup || !p.has_content || (sp_map && std::strcmp(g, "GROUP_MAP") == 0)) continue;
                const XmlElement* gl = p.setup.child("contentChangeSetGroups");
                const XmlElement* sets = p.content.child("contentChangeSets");
                if (!gl || !sets) continue;
                for (const XmlElement* grp : gl->children_named("Item")) {
                    if (lower(grp->child_text("NameHash")) != lower(g)) continue;
                    const XmlElement* names = grp->child("ContentChangeSets");
                    if (!names) continue;
                    for (const XmlElement* n : names->children_named("Item"))
                        for (const XmlElement* cs : sets->children_named("Item"))
                            if (lower(cs->child_text("changeSetName")) == lower(n->text)) { apply(*cs); break; }
                }
            }

        // an archive is mounted when it and every archive holding it are. A pack mounts only the archives its
        // content.xml lists (an archive directly in dlc.rpf that no data file names is not mounted); archives nested
        // deeper and the base game's follow their parent.
        active.assign(archives.size(), -1);
        std::function<int8_t(size_t)> resolve = [&](size_t i) -> int8_t {
            if (active[i] >= 0) return active[i];
            int8_t on = 1;
            if (!logical[i].empty()) {
                auto it = state.find(logical[i]);
                if (it != state.end()) on = it->second ? 1 : 0;
                else if (logical[i].compare(0, 9, "platform:") != 0 && archives[i].parent >= 0) {
                    const std::string& pl = logical[(size_t)archives[i].parent];
                    if (!pl.empty() && pl.back() == '/' && pl.find(":/") == pl.size() - 2) on = 0;   // parent is the pack root "<device>:/"
                }
            }
            if (on && archives[i].parent >= 0) on = resolve((size_t)archives[i].parent);
            return active[i] = on;
        };
        for (size_t i = 0; i < archives.size(); i++) resolve(i);
    }

    bool mounted(const IndexedFile& f) const { return f.archive < active.size() && active[f.archive] == 1; }
};

/// The file the game uses for every name with this extension (the newest copy in a mounted archive), by name hash.
std::vector<IndexedFile> winners(const GameIndex& gi, const char* ext, const MountModel& mm) {
    std::vector<IndexedFile> out;
    const int id = ext_id(gi, ext);
    if (id < 0) return out;
    const auto& files = gi.files();
    for (size_t i = 0; i < files.size();) {
        if ((int)files[i].ext != id) { i++; continue; }
        size_t j = i;
        while (j < files.size() && files[j].name_hash == files[i].name_hash && files[j].ext == files[i].ext) j++;
        if (!mm.disabled.count(files[i].name_hash))
            for (size_t k = j; k-- > i;)   // newest first
                if (gi.rank(files[k]).active() && mm.mounted(files[k])) { out.push_back(files[k]); break; }
        i = j;
    }
    return out;
}

struct StoreItem { Vec3 mn, mx; uint32_t layer; };

/// The BoundsStore section of a cache file: name -> extents.
void read_bounds_store(const Bytes& d, std::unordered_map<uint32_t, StoreItem>& out) {
    static const char kKey[] = "BoundsStore\n";
    auto it = std::search(d.begin(), d.end(), kKey, kKey + sizeof kKey - 1);
    if (it == d.end()) return;
    size_t pos = (size_t)(it - d.begin()) + sizeof kKey - 1;
    if (pos + 4 > d.size()) return;
    uint32_t len;
    std::memcpy(&len, &d[pos], 4);
    pos += 4;
    if (pos + len > d.size()) throw Error("bounds store runs past the end of the cache file");
    for (size_t k = 0; k + 32 <= len; k += 32) {
        float f[6];
        uint32_t name, layer;
        std::memcpy(&name, &d[pos + k], 4);
        std::memcpy(f, &d[pos + k + 4], 24);
        std::memcpy(&layer, &d[pos + k + 28], 4);
        out[name] = {{f[0], f[1], f[2]}, {f[3], f[4], f[5]}, layer};
    }
}

}   // namespace

// ---------------------------------------------------------------------------------------- Impl

struct WorldIndex::Impl {
    const GameIndex* gi = nullptr;
    WorldIndexInfo info;
    std::vector<WorldMap> maps;
    std::vector<WorldMloInstance> mlos;
    std::vector<WorldBounds> bounds;

    mutable std::mutex mu;
    mutable std::list<std::pair<uint32_t, std::shared_ptr<const MapData>>> lru;   // most recent first
    static constexpr size_t kLru = 96;

    void save(const std::string& path, const std::string& key) const {
        Writer w;
        w.raw("RKWI", 4);
        w.pod(WorldIndex::kFormat);
        w.str(key);
        w.pod(info.scan_seconds);
        w.pod(info.archives_invalidated);
        w.pod(info.archives_enabled);
        w.vec(maps);
        w.vec(mlos);
        w.vec(bounds);
        w.pod((uint32_t)info.warnings.size());
        for (auto& s : info.warnings) w.str(s);
        fs::create_directories(fs::path(path).parent_path());
        const std::string tmp = path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary);
            f.write((const char*)w.b.data(), (std::streamsize)w.b.size());
            if (!f) throw Error("cannot write " + tmp);
        }
        fs::rename(tmp, path);
    }

    bool load(const std::string& path, const std::string& key) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        Bytes b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        // read into locals and take them only once they check: a cache cut short or corrupted must leave the index empty
        // for the scan that follows, and every index a row carries must land in its table
        WorldIndexInfo in = info;
        std::vector<WorldMap> ms;
        std::vector<WorldMloInstance> ls;
        std::vector<WorldBounds> bs;
        try {
            Reader r{b.data(), b.data() + b.size()};
            r.need(4);
            if (std::memcmp(r.p, "RKWI", 4) != 0) return false;
            r.p += 4;
            if (r.pod<uint32_t>() != WorldIndex::kFormat || r.str() != key) return false;
            in.scan_seconds = r.pod<double>();
            in.archives_invalidated = r.pod<uint32_t>();
            in.archives_enabled = r.pod<uint32_t>();
            ms = r.vec<WorldMap>();
            ls = r.vec<WorldMloInstance>();
            bs = r.vec<WorldBounds>();
            const uint32_t nw = r.pod<uint32_t>();
            r.need((size_t)nw * 4);   // every string costs at least its length word
            in.warnings.clear();
            for (uint32_t i = 0; i < nw; i++) in.warnings.push_back(r.str());
        } catch (const Error&) {
            return false;
        }
        // a file row must be one the game index holds (the cache key is the game index's, so a whole cache always is)
        auto known = [&](const IndexedFile& x) {
            if (x.ext >= gi->extensions().size()) return false;
            for (const IndexedFile& v : gi->versions(x.name_hash, gi->extensions()[x.ext]))
                if (v.archive == x.archive && v.entry == x.entry && v.dir == x.dir && v.name_id == x.name_id && v.rank == x.rank) return true;
            return false;
        };
        for (const WorldMap& m : ms)
            if (m.parent_map >= (int32_t)ms.size() || m.parent_map < -1 || !known(m.file)) return false;
        for (const WorldMloInstance& l : ls) if (l.map >= ms.size()) return false;
        for (const WorldBounds& x : bs) if (!known(x.file)) return false;
        info = std::move(in);
        maps = std::move(ms);
        mlos = std::move(ls);
        bounds = std::move(bs);
        return true;
    }

    void scan(const WorldIndexOptions& opt) {
        auto say = [&](const std::string& s) { if (opt.progress) opt.progress(s); };
        say("world index: applying the packs' map change sets");
        MountModel mm;
        mm.build(*gi, opt.sp_map, info.warnings);
        info.archives_invalidated = mm.invalidated;
        info.archives_enabled = mm.enabled;
        // IPL groups and interior collision from every mounted manifest (every pack has its own `_manifest.ymf`)
        std::unordered_set<uint32_t> groups;
        std::unordered_map<uint32_t, uint32_t> interior_of;   // ybn name -> MLO name
        {
            const int ymf = ext_id(*gi, ".ymf");
            for (const IndexedFile& f : gi->files()) {
                if ((int)f.ext != ymf || !gi->rank(f).active()) continue;
                try {
                    const Manifest m = read_ymf(gi->extract(f));
                    for (auto& g : m.map_data_groups) groups.insert(g.name);
                    for (auto& in : m.interiors) for (uint32_t b : in.bounds) interior_of[b] = in.name;
                } catch (const std::exception&) {
                    // RBF / RSC7 manifests exist in the archives; they carry no groups an interior tool needs
                }
            }
        }
        // every ymap the game uses, read in parallel
        const std::vector<IndexedFile> files = winners(*gi, ".ymap", mm);
        say("world index: reading " + std::to_string(files.size()) + " map files");
        maps.assign(files.size(), WorldMap{});
        std::vector<std::vector<WorldMloInstance>> inst(files.size());
        std::vector<std::string> errors(files.size());
        std::atomic<size_t> next{0};
        int n_threads = opt.threads > 0 ? opt.threads : (int)std::max(1u, std::thread::hardware_concurrency());
        n_threads = std::min(n_threads, 16);
        auto work = [&] {
            for (;;) {
                const size_t i = next++;
                if (i >= files.size()) break;
                WorldMap& m = maps[i];
                m.name = files[i].name_hash;
                m.file = files[i];
                m.group = groups.count(m.name) ? 1 : 0;
                try {
                    const MapData d = read_ymap(gi->extract(files[i]));
                    m.parent = d.parent;
                    m.flags = d.flags;
                    m.content_flags = d.content_flags;
                    m.streaming_min = d.streaming_extents_min; m.streaming_max = d.streaming_extents_max;
                    m.entities_min = d.entities_extents_min; m.entities_max = d.entities_extents_max;
                    m.entity_count = (uint32_t)d.entities.size();
                    for (size_t k = 0; k < d.entities.size(); k++) {
                        const Entity& e = d.entities[k];
                        if (!e.is_mlo_instance) continue;
                        WorldMloInstance w;
                        w.archetype = e.archetype_name;
                        w.map = (uint32_t)i;
                        w.entity = (uint32_t)k;
                        w.position = e.position;
                        w.orientation = entity_orientation(e);
                        w.box_min = w.box_max = e.position;
                        w.group_id = e.group_id; w.floor_id = e.floor_id; w.inst_flags = e.mlo_inst_flags; w.num_exit_portals = e.num_exit_portals;
                        inst[i].push_back(w);
                        m.mlo_instances++;
                    }
                } catch (const std::exception& ex) {
                    m.readable = 0;
                    errors[i] = gi->path(files[i]) + ": " + ex.what();
                }
            }
        };
        std::vector<std::thread> pool;
        for (int t = 0; t < n_threads; t++) pool.emplace_back(work);
        for (auto& t : pool) t.join();
        for (size_t i = 0; i < files.size(); i++) {
            for (auto& w : inst[i]) mlos.push_back(w);
            if (!errors[i].empty()) info.warnings.push_back(errors[i]);
        }
        // interior boxes: the union of the MLO archetype's rooms, limbo included (its CBaseArchetypeDef box is empty), from
        // its ytyp. The game's interior proxies (gta5_cache_y.dat) carry about this box.
        {
            std::unordered_map<uint32_t, std::shared_ptr<MapTypes>> ytyps;   // by GameIndex file position
            std::unordered_map<uint32_t, std::pair<Vec3, Vec3>> local;       // archetype -> rooms box
            for (WorldMloInstance& w : mlos) {
                auto lb = local.find(w.archetype);
                if (lb == local.end()) {
                    std::pair<Vec3, Vec3> box{{INFINITY, INFINITY, INFINITY}, {-INFINITY, -INFINITY, -INFINITY}};
                    if (auto a = gi->archetype(w.archetype)) {
                        auto& t = ytyps[a->ytyp_file];
                        if (!t) {
                            try { t = std::make_shared<MapTypes>(read_ytyp(gi->extract(gi->ytyp_of(*a)))); }
                            catch (const std::exception& ex) { t = std::make_shared<MapTypes>(); info.warnings.push_back(gi->path(gi->ytyp_of(*a)) + ": " + ex.what()); }
                        }
                        if (const Archetype* m = t->find(w.archetype))
                            for (size_t r = 0; r < m->rooms.size(); r++) {
                                box.first = vmin(box.first, m->rooms[r].bb_min);
                                box.second = vmax(box.second, m->rooms[r].bb_max);
                            }
                    }
                    lb = local.emplace(w.archetype, box).first;
                }
                if (lb->second.first.x <= lb->second.second.x) {
                    w.local_min = lb->second.first;
                    w.local_max = lb->second.second;
                    transform_box(w.local_min, w.local_max, {1, 1, 1}, w.orientation, w.position, w.box_min, w.box_max);
                }
            }
        }
        for (auto& m : maps) {
            if (!m.parent) continue;
            auto it = std::lower_bound(maps.begin(), maps.end(), m.parent, [](const WorldMap& a, uint32_t h) { return a.name < h; });
            if (it != maps.end() && it->name == m.parent) m.parent_map = (int32_t)(it - maps.begin());
        }
        // static collision: the game's bounds stores (base cache, then the packs' in load order; a later one wins)
        say("world index: reading the bounds stores");
        std::unordered_map<uint32_t, StoreItem> store;
        {
            const int dat = ext_id(*gi, ".dat");
            std::vector<IndexedFile> caches;
            for (const IndexedFile& f : gi->files())
                if ((int)f.ext == dat && gi->rank(f).active() && ends_with(gi->file_name(f), "_cache_y.dat")) caches.push_back(f);
            std::stable_sort(caches.begin(), caches.end(), [](const IndexedFile& a, const IndexedFile& b) { return a.rank < b.rank; });
            for (const IndexedFile& c : caches) {
                try { read_bounds_store(gi->extract(c), store); }
                catch (const std::exception& ex) { info.warnings.push_back(gi->path(c) + ": " + ex.what()); }
            }
        }
        for (const IndexedFile& f : winners(*gi, ".ybn", mm)) {
            WorldBounds b;
            b.name = f.name_hash;
            b.file = f;
            auto io = interior_of.find(f.name_hash);
            if (io != interior_of.end()) b.interior = io->second;
            auto it = store.find(f.name_hash);
            if (it != store.end()) { b.box_min = it->second.mn; b.box_max = it->second.mx; b.layer = it->second.layer; }
            else {
                b.cached = 0;
                try {
                    const BoundPtr bp = read_ybn(gi->extract(f));
                    if (!bp) continue;
                    b.box_min = bp->box_min; b.box_max = bp->box_max;
                } catch (const std::exception& ex) {
                    info.warnings.push_back(gi->path(f) + ": " + ex.what());
                    continue;
                }
            }
            bounds.push_back(b);
        }
    }
};

// ---------------------------------------------------------------------------------------- WorldIndex

WorldIndex WorldIndex::open(const GameIndex& gi, const WorldIndexOptions& opt) {
    const auto t0 = std::chrono::steady_clock::now();
    WorldIndex w;
    w.d_ = std::make_shared<Impl>();
    Impl& d = *w.d_;
    d.gi = &gi;
    std::string folder = opt.cache_folder;
    if (folder.empty()) folder = gi.info().cache_path.empty() ? (fs::path(home_dir()) / ".ragekit" / "index").string() : fs::path(gi.info().cache_path).parent_path().string();
    const std::string key = gi.info().key;
    d.info.cache_path = (fs::path(folder) / ("world_" + key + (opt.sp_map ? "_sp" : "_mp") + ".bin")).string();
    if (opt.use_cache && !opt.rebuild && !key.empty() && d.load(d.info.cache_path, key)) {
        d.info.from_cache = true;
    } else {
        d.scan(opt);
        d.info.scan_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (opt.use_cache && !key.empty()) {
            try { d.save(d.info.cache_path, key); }
            catch (const std::exception& ex) { d.info.warnings.push_back(std::string("world index cache not written: ") + ex.what()); }
        }
    }
    d.info.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return w;
}

const WorldIndexInfo& WorldIndex::info() const { return d_->info; }
const std::vector<WorldMap>& WorldIndex::maps() const { return d_->maps; }
const std::vector<WorldMloInstance>& WorldIndex::mlo_instances() const { return d_->mlos; }
const std::vector<WorldBounds>& WorldIndex::bounds() const { return d_->bounds; }

int WorldIndex::find_map(uint32_t name) const {
    const auto& m = d_->maps;
    auto it = std::lower_bound(m.begin(), m.end(), name, [](const WorldMap& a, uint32_t h) { return a.name < h; });
    return it != m.end() && it->name == name ? (int)(it - m.begin()) : -1;
}

std::vector<uint32_t> WorldIndex::maps_in(const Vec3& mn, const Vec3& mx, bool groups) const {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < d_->maps.size(); i++) {
        const WorldMap& m = d_->maps[i];
        if (!m.readable || !m.entity_count || (m.group && !groups)) continue;
        if (overlaps(m.entities_min, m.entities_max, mn, mx)) out.push_back((uint32_t)i);
    }
    return out;
}

std::shared_ptr<const MapData> WorldIndex::map_data(uint32_t map) const {
    {
        std::lock_guard<std::mutex> g(d_->mu);
        for (auto it = d_->lru.begin(); it != d_->lru.end(); ++it)
            if (it->first == map) {
                d_->lru.splice(d_->lru.begin(), d_->lru, it);
                return it->second;
            }
    }
    auto md = std::make_shared<const MapData>(read_ymap(d_->gi->extract(d_->maps.at(map).file)));
    std::lock_guard<std::mutex> g(d_->mu);
    d_->lru.emplace_front(map, md);
    if (d_->lru.size() > Impl::kLru) d_->lru.pop_back();
    return md;
}

std::vector<WorldEntity> WorldIndex::entities_in(const Vec3& mn, const Vec3& mx, const WorldQuery& q) const {
    std::vector<WorldEntity> out;
    for (uint32_t mi : maps_in(mn, mx, q.groups)) {
        std::shared_ptr<const MapData> md;
        try { md = map_data(mi); } catch (const std::exception&) { continue; }
        for (size_t k = 0; k < md->entities.size(); k++) {
            const Entity& e = md->entities[k];
            if (e.is_mlo_instance ? !q.mlo_instances : !(q.lod_mask & (1u << std::clamp(e.lod_level, 0, 31)))) continue;
            WorldEntity w;
            w.map = mi;
            w.index = (uint32_t)k;
            w.orientation = entity_orientation(e);
            w.box_min = w.box_max = e.position;
            if (e.is_mlo_instance) {   // an MLO archetype has no box of its own: the rooms' box from the index
                auto it = std::lower_bound(d_->mlos.begin(), d_->mlos.end(), std::make_pair(mi, (uint32_t)k), [](const WorldMloInstance& m, const std::pair<uint32_t, uint32_t>& key) {
                    return m.map < key.first || (m.map == key.first && m.entity < key.second);
                });
                if (it != d_->mlos.end() && it->map == mi && it->entity == k) { w.box_min = it->box_min; w.box_max = it->box_max; w.archetype_known = true; }
            } else if (auto a = d_->gi->archetype(e.archetype_name)) {
                w.archetype_known = true;
                transform_box(a->bb_min, a->bb_max, {e.scale_xy, e.scale_xy, e.scale_z}, w.orientation, e.position, w.box_min, w.box_max);
            }
            if (!overlaps(w.box_min, w.box_max, mn, mx)) continue;
            w.def = e;
            out.push_back(std::move(w));
        }
    }
    return out;
}

std::vector<WorldEntity> WorldIndex::entities_at(const Vec3& p, const WorldQuery& q) const {
    std::vector<WorldEntity> out = entities_in(p, p, q);
    auto volume = [](const WorldEntity& e) { const Vec3 s = e.box_max - e.box_min; return s.x * s.y * s.z; };
    std::stable_sort(out.begin(), out.end(), [&](const WorldEntity& a, const WorldEntity& b) { return volume(a) < volume(b); });
    return out;
}

std::vector<uint32_t> WorldIndex::bounds_in(const Vec3& mn, const Vec3& mx, bool interiors) const {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < d_->bounds.size(); i++) {
        const WorldBounds& b = d_->bounds[i];
        if (b.interior && !interiors) continue;
        if (overlaps(b.box_min, b.box_max, mn, mx)) out.push_back((uint32_t)i);
    }
    return out;
}

} // namespace rk
