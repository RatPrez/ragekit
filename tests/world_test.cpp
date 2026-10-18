// world_test: the world index against the game install (skipped without one).
//   - counts: map files, MLO instances, collision files (with the share the game's bounds store lists)
//   - the rotation convention of MLO instances, decided by the game's own interior proxies (gta5_cache_y.dat,
//     CInteriorProxy: position, orientation and world box the game computed for every interior instance)
//   - a corrupted copy of the cache is not taken
//   - queries at known places: Michael's house (the v_michael interior inside the building), Franklin's house
//   - with --oracle <tsv> (`oracle world <game> <tsv>`): the ymaps / ybns CodeWalker loads with every DLC pack, name by
//     name and archive by archive (CodeWalker's rules differ in known ways: listed, not failed)
// Usage: rk_world_test [--rebuild] [--oracle <tsv>]. Exit 0 = every check passed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include "ragekit/bounds.h"
#include "ragekit/gameindex.h"
#include "ragekit/hash.h"
#include "ragekit/rsc.h"
#include "ragekit/world.h"

using namespace rk;
namespace fs = std::filesystem;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("%-70s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

struct Proxy { uint32_t name, parent; Vec3 pos; Quat q; Vec3 bmin, bmax; };

static std::vector<Proxy> read_proxies(const Bytes& d) {
    std::vector<Proxy> out;
    static const char kKey[] = "CInteriorProxy\n";
    auto it = std::search(d.begin(), d.end(), kKey, kKey + sizeof kKey - 1);
    if (it == d.end()) return out;
    size_t pos = (size_t)(it - d.begin()) + sizeof kKey - 1;
    uint32_t len;
    std::memcpy(&len, &d[pos], 4);
    pos += 4;
    for (size_t k = 0; k + 104 <= len; k += 104) {
        const uint8_t* p = &d[pos + k];
        Proxy x;
        std::memcpy(&x.name, p + 12, 4);
        std::memcpy(&x.parent, p + 16, 4);
        float f[13];
        std::memcpy(f, p + 20, sizeof f);
        x.pos = {f[0], f[1], f[2]};
        x.q = {f[3], f[4], f[5], f[6]};
        x.bmin = {f[7], f[8], f[9]};
        x.bmax = {f[10], f[11], f[12]};
        out.push_back(x);
    }
    return out;
}

static float box_err(const Vec3& a0, const Vec3& a1, const Vec3& b0, const Vec3& b1) {
    const Vec3 d0 = a0 - b0, d1 = a1 - b1;
    return std::fabs(d0.x) + std::fabs(d0.y) + std::fabs(d0.z) + std::fabs(d1.x) + std::fabs(d1.y) + std::fabs(d1.z);
}

int main(int argc, char** argv) {
    const std::string game = GameIndex::default_game_dir();
    if (game.empty()) { std::printf("no game install: skipped\n"); return 0; }
    GameIndex gi = GameIndex::open(game);
    WorldIndexOptions o;
    std::string oracle;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--rebuild") == 0) o.rebuild = true;
        else if (std::strcmp(argv[i], "--oracle") == 0 && i + 1 < argc) oracle = argv[++i];
    }
    o.progress = [](const std::string& s) { std::fprintf(stderr, "%s\n", s.c_str()); };
    WorldIndex w = WorldIndex::open(gi, o);
    const auto& info = w.info();
    size_t readable = 0, grouped = 0, ents = 0, cached = 0, interior = 0;
    for (auto& m : w.maps()) { readable += m.readable; grouped += m.group; ents += m.entity_count; }
    for (auto& b : w.bounds()) { cached += b.cached; interior += b.interior != 0; }
    std::printf("world index: %s in %.2f s (scan %.1f s), %s\n", info.from_cache ? "cache" : "scanned", info.seconds, info.scan_seconds, info.cache_path.c_str());
    std::printf("  maps %zu (readable %zu, IPL groups %zu), entities %zu, MLO instances %zu\n", w.maps().size(), readable, grouped, ents, w.mlo_instances().size());
    std::printf("  collision files %zu (bounds store %zu, interior %zu), warnings %zu\n", w.bounds().size(), cached, interior, info.warnings.size());
    for (size_t i = 0; i < info.warnings.size() && i < 6; i++) std::printf("    %s\n", info.warnings[i].c_str());
    std::printf("  map change sets: %u archives turned off, %u turned on\n", info.archives_invalidated, info.archives_enabled);
    check(w.maps().size() > 5000 && readable + 10 >= w.maps().size(), "every ymap the game loads indexed and read");
    check(w.mlo_instances().size() > 500, "MLO instances found");
    check(w.bounds().size() > 10000 && cached * 10 > w.bounds().size() * 9, "collision files with the game's bounds store extents");

    // --- the MLO instance rotation, decided by the game's interior proxies
    std::vector<Proxy> proxies;
    if (auto c = gi.resolve_file("gta5_cache_y", ".dat")) proxies = read_proxies(gi.extract(*c));
    size_t matched = 0, same = 0, conj = 0, rotated = 0, box_same = 0, box_conj = 0;
    for (const Proxy& p : proxies) {
        for (const WorldMloInstance& m : w.mlo_instances()) {
            if (m.archetype != p.name || (m.position - p.pos).length() > 0.05f) continue;
            matched++;
            const Quat& q = m.orientation;   // as stored (the index's convention for instances)
            const float ds = std::fabs(q.x - p.q.x) + std::fabs(q.y - p.q.y) + std::fabs(q.z - p.q.z) + std::fabs(q.w - p.q.w);
            const float dc = std::fabs(-q.x - p.q.x) + std::fabs(-q.y - p.q.y) + std::fabs(-q.z - p.q.z) + std::fabs(q.w - p.q.w);
            if (ds < 1e-3f) same++;
            else if (dc < 1e-3f) conj++;
            if (std::fabs(std::fabs(q.w) - 1.f) < 1e-3f || m.local_min.x >= m.local_max.x) break;   // no rotation (or no rooms): the box says nothing
            rotated++;
            Vec3 s0, s1, c0, c1;
            transform_box(m.local_min, m.local_max, {1, 1, 1}, q, m.position, s0, s1);
            transform_box(m.local_min, m.local_max, {1, 1, 1}, Quat{-q.x, -q.y, -q.z, q.w}, m.position, c0, c1);
            const float es = box_err(s0, s1, p.bmin, p.bmax), ec = box_err(c0, c1, p.bmin, p.bmax);
            if (es + 0.5f < ec) box_same++;
            else if (ec + 0.5f < es) box_conj++;
            break;
        }
    }
    std::printf("  interior proxies %zu, matched to instances %zu: orientation = stored %zu, = inverse %zu; rotated %zu: box fits stored %zu, inverse %zu\n",
                proxies.size(), matched, same, conj, rotated, box_same, box_conj);
    check(matched > 100, "interior proxies matched to MLO instances");
    check(box_same > 10 * std::max<size_t>(box_conj, 1), "rotated interiors: the stored quaternion is the world orientation");

    // --- queries at known places
    auto name = [&](uint32_t h) { return gi.name(h); };
    {
        const Vec3 p{-806.f, 175.f, 76.f};   // Michael's house, ground floor
        auto at = w.entities_at(p);
        bool mlo = false, building = false;
        for (auto& e : at) {
            if (e.def.is_mlo_instance && name(e.def.archetype_name) == "v_michael") mlo = true;
            if (!e.def.is_mlo_instance && e.def.lod_level == 0) building = true;
        }
        std::printf("  at Michael's house: %zu entities", at.size());
        for (size_t i = 0; i < at.size() && i < 6; i++) std::printf("%s %s", i ? "," : ":", name(at[i].def.archetype_name).c_str());
        std::printf("\n");
        check(mlo, "Michael's house: v_michael interior instance at the point");
        check(building, "Michael's house: an HD building at the point");
        auto b = w.bounds_in(p - Vec3{5, 5, 5}, p + Vec3{5, 5, 5});
        size_t interior = 0;
        for (auto& wb : w.bounds()) if (wb.interior == joaat_lower("v_michael")) interior++;
        std::printf("  collision files near it: %zu map; v_michael's own: %zu\n", b.size(), interior);
        check(!b.empty() && interior >= 1, "Michael's house: map collision around it, the interior's collision listed");
        // the collision as triangles: every vertex inside the box the game's bounds store gives the file (composite
        // children through their transforms), and some triangles within the clip box around the house
        size_t tris = 0, outside = 0, clipped = 0;
        const AABB clip{{p.x - 15, p.y - 15, p.z - 10, 0}, {p.x + 15, p.y + 15, p.z + 10, 0}};
        for (uint32_t bi : b) {
            const WorldBounds& wb = w.bounds()[bi];
            const BoundPtr bp = read_ybn(gi.extract(wb.file));
            for (const BoundTriangle& t : bound_triangles(*bp)) {
                tris++;
                for (const Vec3& v : t.v)
                    if (v.x < wb.box_min.x - 1 || v.y < wb.box_min.y - 1 || v.z < wb.box_min.z - 1 || v.x > wb.box_max.x + 1 || v.y > wb.box_max.y + 1 || v.z > wb.box_max.z + 1) { outside++; break; }
            }
            clipped += bound_triangles(*bp, 8, &clip).size();
        }
        std::printf("  their triangles: %zu (%zu outside the store's box), %zu within 15 m of the point\n", tris, outside, clipped);
        check(tris > 1000 && outside == 0 && clipped > 50 && clipped < tris, "Michael's house: collision triangles in world space");
    }
    {
        const Vec3 p{-9.f, 520.f, 175.f};   // Franklin's house in Vinewood Hills
        auto at = w.entities_at(p);
        bool mlo = false;
        for (auto& e : at) if (e.def.is_mlo_instance && name(e.def.archetype_name) == "v_franklinshouse") mlo = true;
        check(mlo, "Franklin's house: v_franklinshouse interior instance at the point");
    }
    if (!oracle.empty()) {   // CodeWalker's world, name by name
        auto low = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
        std::map<std::string, std::string> cw[2], rk[2];   // 0 ymap, 1 ybn: name -> path
        std::ifstream f(oracle);
        for (std::string line; std::getline(f, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t a = line.find('\t'), b = line.find('\t', a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            std::string n = line.substr(a + 1, b - a - 1);
            n = n.substr(0, n.rfind('.'));
            cw[line.compare(0, a, "ybn") == 0][n] = low(line.substr(b + 1));
        }
        auto stem = [&](const IndexedFile& f) { const std::string n = gi.file_name(f); return low(n.substr(0, n.rfind('.'))); };
        for (auto& m : w.maps()) rk[0][stem(m.file)] = low(gi.path(m.file));
        for (auto& b : w.bounds()) rk[1][stem(b.file)] = low(gi.path(b.file));
        for (int k = 0; k < 2; k++) {
            size_t only_cw = 0, only_rk = 0, same = 0, other = 0;
            std::vector<std::string> ex_cw, ex_rk, ex_other;
            for (auto& [n, p] : cw[k]) {
                auto it = rk[k].find(n);
                if (it == rk[k].end()) { only_cw++; if (ex_cw.size() < 8) ex_cw.push_back(p); }
                else if (it->second == p) same++;
                else { other++; if (ex_other.size() < 8) ex_other.push_back(n + ": cw " + p + " | rk " + it->second); }
            }
            for (auto& [n, p] : rk[k]) if (!cw[k].count(n)) { only_rk++; if (ex_rk.size() < 8) ex_rk.push_back(p); }
            std::printf("  oracle %s: same file %zu, other copy %zu, only CodeWalker %zu, only ragekit %zu\n", k ? "ybn" : "ymap", same, other, only_cw, only_rk);
            for (auto& e : ex_other) std::printf("    other: %s\n", e.c_str());
            for (auto& e : ex_cw) std::printf("    only cw: %s\n", e.c_str());
            for (auto& e : ex_rk) std::printf("    only rk: %s\n", e.c_str());
        }
    }

    // a copy of the cache whose last MLO instance names a map past the table is not taken (it crashed the first query)
    if (info.from_cache && !w.mlo_instances().empty()) {
        const fs::path dir = fs::temp_directory_path() / "rk_world_test_cache";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        const fs::path copy = dir / fs::path(info.cache_path).filename();
        Bytes bin = rsc::read_file(info.cache_path);
        // the MLO rows follow the maps: find the last row by its archetype and map index
        const WorldMloInstance& last = w.mlo_instances().back();
        size_t at = SIZE_MAX;
        for (size_t i = bin.size() - sizeof(WorldMloInstance); i-- > 0;)
            if (std::memcmp(&bin[i], &last, sizeof(WorldMloInstance)) == 0) { at = i; break; }
        check(at != SIZE_MAX, "world cache: the last MLO instance's row is found in the cache");
        if (at != SIZE_MAX) {
            const uint32_t bad = (uint32_t)w.maps().size() + 100;
            std::memcpy(&bin[at + offsetof(WorldMloInstance, map)], &bad, 4);
            rsc::write_file(copy.string(), bin);
            WorldIndexOptions o2 = o;
            o2.cache_folder = dir.string();
            o2.progress = {};
            WorldIndex w2 = WorldIndex::open(gi, o2);
            check(!w2.info().from_cache && w2.mlo_instances().size() == w.mlo_instances().size(),
                  "world cache: a row naming a map past the table makes the cache unusable (scanned again)");
        }
        fs::remove_all(dir, ec);
    }
    std::printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
