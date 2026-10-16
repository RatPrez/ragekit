// rk index | find | pull: the game asset index (include/ragekit/gameindex.h).
#include <algorithm>
#include <charconv>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "ragekit/ragekit.h"
#include "ragekit/gameindex.h"
#include "ragekit/world.h"
#include "ragekit/occluder.h"

using namespace rk;
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

struct Common {
    std::string game;
    bool verbose = false;
    bool rebuild = false;
    size_t limit = 0;
    bool have_limit = false;
    std::string type;
    int threads = 0;
    std::vector<std::string> rest;
};

Common parse(const std::vector<std::string>& args) {
    Common c;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--game" && i + 1 < args.size()) c.game = args[++i];
        else if (a == "--type" && i + 1 < args.size()) c.type = lower(args[++i]);
        else if (a == "--limit" && i + 1 < args.size()) { c.limit = (size_t)std::atoll(args[++i].c_str()); c.have_limit = true; }
        else if (a == "--threads" && i + 1 < args.size()) c.threads = std::atoi(args[++i].c_str());
        else if (a == "--rebuild") c.rebuild = true;
        else if (a == "-v") c.verbose = true;
        else c.rest.push_back(a);
    }
    return c;
}

GameIndex open_index(const Common& c, const std::string& game_in, bool rebuild) {
    std::string game = game_in.empty() ? c.game : game_in;
    if (game.empty()) game = GameIndex::default_game_dir();
    if (game.empty()) throw Error("no game folder: pass --game <dir> or set RAGEKIT_GAME");
    GameIndexOptions o;
    o.rebuild = rebuild;
    o.threads = c.threads;
    o.progress = [](const std::string& s) { std::fprintf(stderr, "%s\n", s.c_str()); std::fflush(stderr); };
    return GameIndex::open(game, o);
}

const char* kind_name(uint8_t k) { return k == IndexedArchetype::Mlo ? "mlo" : k == IndexedArchetype::Time ? "time" : "base"; }
const char* asset_name_of(int t) {
    switch (t) { case 1: return "fragment"; case 2: return "drawable"; case 3: return "dictionary"; case 4: return "assetless"; default: return "?"; }
}

std::string win(const std::string& s) { std::string o = s; for (auto& c : o) if (c == '/') c = '\\'; return o; }

std::string num(float f) {
    char buf[32];
    auto r = std::to_chars(buf, buf + sizeof buf, f);
    return std::string(buf, r.ptr);
}

void print_info(const GameIndex& gi, bool verbose) {
    const auto& inf = gi.info();
    std::printf("game folder   %s\n", inf.game_dir.c_str());
    std::printf("exe version   %s\n", inf.exe_version.c_str());
    std::printf("cache         %s (%.1f MB, key %s)\n", inf.cache_path.c_str(), (double)inf.cache_bytes / 1048576.0, inf.key.c_str());
    std::printf("opened        %s in %.2f s (the scan that built it took %.1f s)\n", inf.from_cache ? "from the cache" : "by scanning", inf.seconds, inf.scan_seconds);
    size_t nested = 0, top = 0;
    for (auto& a : gi.archives()) (a.parent < 0 ? top : nested)++;
    std::printf("archives      %zu (%zu on disk, %zu nested)\n", gi.archives().size(), top, nested);
    std::printf("files         %zu\n", gi.files().size());
    std::map<std::string, size_t> by_ext;
    size_t ytyps = 0;
    for (auto& f : gi.files()) by_ext[gi.extension(f)]++;
    ytyps = by_ext[".ytyp"];
    std::vector<std::pair<size_t, std::string>> ev;
    for (auto& kv : by_ext) ev.emplace_back(kv.second, kv.first);
    std::sort(ev.rbegin(), ev.rend());
    std::printf("  by type    ");
    for (size_t i = 0; i < ev.size() && i < 14; i++) std::printf(" %s %zu", ev[i].second.c_str(), ev[i].first);
    std::printf("\n");
    size_t base = 0, time = 0, mlo = 0, uniq = 0;
    uint32_t last = 0;
    bool first = true;
    for (auto& a : gi.archetypes()) {
        (a.kind == IndexedArchetype::Mlo ? mlo : a.kind == IndexedArchetype::Time ? time : base)++;
        if (first || a.name != last) uniq++;
        first = false; last = a.name;
    }
    std::printf("ytyp files    %zu\n", ytyps);
    std::printf("archetypes    %zu definitions (%zu base, %zu time, %zu mlo), %zu distinct names\n", gi.archetypes().size(), base, time, mlo, uniq);
    std::printf("load order    (lowest first)\n");
    for (auto& r : gi.ranks()) std::printf("  %s\n", r.label().c_str());
    std::printf("warnings      %zu\n", inf.warnings.size());
    size_t shown = 0;
    for (auto& w : inf.warnings) {
        if (!verbose && shown++ >= 10) { std::printf("  ... (-v for all)\n"); break; }
        std::printf("  %s\n", w.c_str());
    }
}

int cmd_index(const std::vector<std::string>& args) {
    Common c = parse(args);
    if (c.rest.empty()) return 2;
    const std::string sub = c.rest[0];
    if (sub == "build") {
        auto t0 = std::chrono::steady_clock::now();
        GameIndex gi = open_index(c, c.rest.size() > 1 ? c.rest[1] : "", true);
        std::printf("built in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        print_info(gi, c.verbose);
        return 0;
    }
    if (sub == "info") {
        GameIndex gi = open_index(c, c.rest.size() > 1 ? c.rest[1] : "", c.rebuild);
        print_info(gi, c.verbose);
        return 0;
    }
    if (sub == "versions" && c.rest.size() >= 2) {
        GameIndex gi = open_index(c, "", false);
        std::string ext = c.rest.size() > 2 ? c.rest[2] : ".ydr";
        uint32_t h = hash_or_name(c.rest[1]);
        auto v = gi.versions(h, ext);
        for (auto& f : v) std::printf("%-44s %s  %u bytes\n", gi.rank(f).label().c_str(), gi.path(f).c_str(), f.size);
        if (v.empty()) std::printf("no %s named %s\n", ext.c_str(), c.rest[1].c_str());
        return 0;
    }
    if (sub == "explain" && c.rest.size() == 2) {
        // lines "kind<TAB>codewalker pick<TAB>ragekit pick" (paths, optionally "ydr:" prefixed, "-" for none), written by
        // `oracle index`: says whether CodeWalker's copy is a copy ragekit knows and ranks below the one ragekit chose
        GameIndex gi = open_index(c, "", false);
        std::FILE* f = std::fopen(c.rest[1].c_str(), "rb");
        if (!f) throw Error("cannot read " + c.rest[1]);
        std::map<std::string, size_t> cats;
        std::vector<std::string> bad;
        size_t total = 0;
        char buf[4096];
        while (std::fgets(buf, sizeof buf, f)) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            std::vector<std::string> p;
            size_t s = 0;
            for (;;) { size_t t = line.find('\t', s); p.push_back(line.substr(s, t == std::string::npos ? t : t - s)); if (t == std::string::npos) break; s = t + 1; }
            if (p.size() != 3) continue;
            total++;
            auto strip = [](std::string x) { size_t k = x.find(':'); return k != std::string::npos && k <= 3 ? x.substr(k + 1) : x; };
            std::string cw = strip(p[1]), rkp = strip(p[2]);
            if (cw == "-" || rkp == "-") { cats[cw == "-" ? "CodeWalker found no file, ragekit did" : "ragekit found no file, CodeWalker did"]++; if (cw != "-") bad.push_back(line); continue; }
            size_t sl = rkp.rfind('/'), dt = rkp.rfind('.');
            std::string stem = rkp.substr(sl + 1, dt - sl - 1), ext = rkp.substr(dt);
            auto v = gi.versions(joaat_lower(stem), ext);
            int icw = -1, irk = -1;
            for (size_t i = 0; i < v.size(); i++) {
                std::string pp = lower(gi.path(v[i]));
                if (pp == cw) icw = (int)i;
                if (pp == rkp) irk = (int)i;
            }
            if (icw < 0 || irk < 0) { cats["UNEXPLAINED: a pick is not a known copy"]++; bad.push_back(line); continue; }
            const LoadRank &rc = gi.rank(v[(size_t)icw]), &rr = gi.rank(v[(size_t)irk]);
            std::string key;
            if (rc.tier == rr.tier && rc.order == rr.order && rc.index == rr.index && rc.patch == rr.patch) key = "UNEXPLAINED: same rank";
            else if (rc.tier > rr.tier || (rc.tier == rr.tier && (rc.order > rr.order || (rc.order == rr.order && rc.index > rr.index)))) key = "UNEXPLAINED: CodeWalker's copy ranks higher";
            else if (rc.tier == 3 && rr.tier == 3 && rc.index == rr.index && rr.patch > rc.patch) key = "ragekit later: the title update's dlc_patch copy of the same pack";
            else if (rr.tier == 3 && rc.tier == 3) key = "ragekit later: the pack comes later in dlclist/setup2 order than CodeWalker's (alphabetical scan order)";
            else key = "ragekit later: " + std::string(rr.tier == 3 ? "a DLC pack over " : "update.rpf/update2.rpf over ") + (rc.tier == 0 ? "the base game" : rc.tier == 1 ? "update.rpf" : rc.tier == 2 ? "update2.rpf" : "an unmounted pack");
            cats[key]++;
            if (key.rfind("UNEXPLAINED", 0) == 0) bad.push_back(line);
        }
        std::fclose(f);
        std::printf("%zu differences:\n", total);
        for (auto& kv : cats) std::printf("  %6zu  %s\n", kv.second, kv.first.c_str());
        for (size_t i = 0; i < bad.size() && i < 15; i++) std::printf("    %s\n", bad[i].c_str());
        return 0;
    }
    if (sub == "dump" && c.rest.size() == 3) {
        // for the oracle (tests/oracle `index`): one row per archetype the game would use, one per ytyp
        GameIndex gi = open_index(c, c.rest[1], false);
        std::FILE* f = std::fopen(c.rest[2].c_str(), "wb");
        if (!f) throw Error("cannot write " + c.rest[2]);
        auto pth = [&](const std::optional<IndexedFile>& o) { return o ? lower(gi.path(*o)) : std::string("-"); };
        std::map<uint32_t, size_t> ytyp_counts;
        for (size_t i = 0; i < gi.files().size(); i++)
            if (gi.extension(gi.files()[i]) == ".ytyp") ytyp_counts[(uint32_t)i] = gi.archetype_count((uint32_t)i);
        {
            std::map<std::string, size_t> by_ext;
            for (auto& x : gi.files()) by_ext[gi.extension(x)]++;
            std::fprintf(f, "I\tfiles\t%zu\n", gi.files().size());
            for (auto& kv : by_ext) std::fprintf(f, "E\t%s\t%zu\n", kv.first.c_str(), kv.second);
        }
        for (auto& kv : ytyp_counts) std::fprintf(f, "Y\t%s\t%zu\n", lower(gi.path(gi.files()[kv.first])).c_str(), kv.second);
        const auto& A = gi.archetypes();
        for (size_t i = 0; i < A.size();) {
            size_t j = i;
            while (j < A.size() && A[j].name == A[i].name) ++j;
            const IndexedArchetype& a = A[j - 1];
            i = j;
            if (!gi.ranks()[a.rank].active()) continue;
            ResolvedDrawable rd = gi.resolve_drawable(a);
            std::string draw = rd.type == ResolvedDrawable::None ? "-" : std::string(rd.type == ResolvedDrawable::Ydr ? "ydr:" : rd.type == ResolvedDrawable::Yft ? "yft:" : "ydd:") + lower(gi.path(*rd.file));
            std::fprintf(f, "A\t%08x\t%s\t%s\t%s\t%08x\t%08x\t%08x\t%d\t%s\t%s\n", a.name, kind_name(a.kind), num(a.lod_dist).c_str(),
                         lower(gi.path(gi.ytyp_of(a))).c_str(), a.texture_dictionary, a.drawable_dictionary, a.asset_name, (int)a.asset_type,
                         draw.c_str(), a.texture_dictionary ? pth(gi.resolve_file(a.texture_dictionary, ".ytd")).c_str() : "-");
        }
        std::fclose(f);
        std::printf("wrote %s\n", c.rest[2].c_str());
        return 0;
    }
    return 2;
}

void print_archetype(const GameIndex& gi, const IndexedArchetype& a) {
    ResolvedDrawable r = gi.resolve_drawable(a);
    std::string model = r.type == ResolvedDrawable::None ? "(no model found)" : gi.file_name(*r.file);
    std::printf("archetype %-36s %-4s %-10s lod %-6s txd %-32s model %s\n", gi.name(a.name).c_str(), kind_name(a.kind), asset_name_of(a.asset_type),
                num(a.lod_dist).c_str(), a.texture_dictionary ? gi.name(a.texture_dictionary).c_str() : "-", model.c_str());
    std::printf("          bb %s,%s,%s .. %s,%s,%s  flags %u  in %s  [%s]\n", num(a.bb_min.x).c_str(), num(a.bb_min.y).c_str(), num(a.bb_min.z).c_str(),
                num(a.bb_max.x).c_str(), num(a.bb_max.y).c_str(), num(a.bb_max.z).c_str(), a.flags, gi.file_name(gi.ytyp_of(a)).c_str(),
                gi.ranks()[a.rank].label().c_str());
}

int cmd_find(const std::vector<std::string>& args) {
    Common c = parse(args);
    if (c.rest.size() != 1) return 2;
    GameIndex gi = open_index(c, "", c.rebuild);
    const std::string& q = c.rest[0];
    size_t limit = c.have_limit ? c.limit : 30;
    static const std::set<std::string> file_types = {"ydr", "ytd", "yft", "ybn", "ytyp", "ydd", "ymap"};
    bool want_arch = c.type.empty() || c.type == "archetype";
    if (!c.type.empty() && c.type != "archetype" && !file_types.count(c.type)) { std::fprintf(stderr, "error: unknown --type %s\n", c.type.c_str()); return 2; }
    size_t n = 0;
    if (want_arch) {
        auto v = gi.find_archetypes(q, limit);
        for (auto& a : v) print_archetype(gi, a);
        n += v.size();
        if (v.size() >= limit && limit) std::printf("(limited to %zu archetypes; --limit N)\n", limit);
    }
    if (c.type.empty() || c.type != "archetype") {
        std::vector<std::string> exts;
        if (c.type.empty()) exts = {"ydr", "yft", "ydd", "ytd", "ybn", "ytyp"};
        else exts = {c.type};
        for (auto& e : exts) {
            auto v = gi.find_files(q, "." + e, limit);
            for (auto& f : v) std::printf("%-5s %-40s %9u bytes  %s\n", e.c_str(), gi.file_name(f).c_str(), f.size, gi.path(f).c_str());
            n += v.size();
            if (v.size() >= limit && limit) std::printf("(limited to %zu .%s files; --limit N)\n", limit, e.c_str());
        }
    }
    if (n == 0) std::printf("nothing matches '%s'\n", q.c_str());
    return 0;
}

// ---- the archetype as CodeWalker-style ytyp XML ----

struct Xml {
    const GameIndex& gi;
    std::string out;
    int depth = 0;
    void line(const std::string& s) { out.append((size_t)depth, ' ').append(s).append("\n"); }
    std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else o += c; }
        return o;
    }
    std::string hname(uint32_t h) { return h ? esc(gi.name(h)) : std::string(); }
    void value(const char* tag, const std::string& v) { line(std::string("<") + tag + " value=\"" + v + "\" />"); }
    void text(const char* tag, const std::string& v) { line(v.empty() ? std::string("<") + tag + " />" : std::string("<") + tag + ">" + v + "</" + tag + ">"); }
    void vec3(const char* tag, const Vec3& v) { line(std::string("<") + tag + " x=\"" + num(v.x) + "\" y=\"" + num(v.y) + "\" z=\"" + num(v.z) + "\" />"); }
    void vec4(const char* tag, const Vec4& v) { line(std::string("<") + tag + " x=\"" + num(v.x) + "\" y=\"" + num(v.y) + "\" z=\"" + num(v.z) + "\" w=\"" + num(v.w) + "\" />"); }

    static const char* asset_type(int t) {
        switch (t) { case 1: return "ASSET_TYPE_FRAGMENT"; case 2: return "ASSET_TYPE_DRAWABLE"; case 3: return "ASSET_TYPE_DRAWABLEDICTIONARY"; case 4: return "ASSET_TYPE_ASSETLESS"; default: return "ASSET_TYPE_UNINITIALIZED"; }
    }
    static const char* lod_level(int l) {
        static const char* n[] = {"LODTYPES_DEPTH_HD", "LODTYPES_DEPTH_LOD", "LODTYPES_DEPTH_SLOD1", "LODTYPES_DEPTH_SLOD2", "LODTYPES_DEPTH_SLOD3", "LODTYPES_DEPTH_ORPHANHD", "LODTYPES_DEPTH_SLOD4"};
        return l >= 0 && l < 7 ? n[l] : "LODTYPES_DEPTH_HD";
    }
    static const char* priority(int p) {
        static const char* n[] = {"PRI_REQUIRED", "PRI_OPTIONAL_HIGH", "PRI_OPTIONAL_MEDIUM", "PRI_OPTIONAL_LOW"};
        return p >= 0 && p < 4 ? n[p] : "PRI_REQUIRED";
    }
    void uints(const char* tag, const std::vector<uint32_t>& v, const char* attr = nullptr) {
        std::string t = std::string("<") + tag + (attr ? std::string(" ") + attr : "") + ">";
        if (v.empty()) { line(std::string("<") + tag + (attr ? std::string(" ") + attr : "") + " />"); return; }
        std::string body;
        for (size_t i = 0; i < v.size(); i++) body += (i ? " " : "") + std::to_string(v[i]);
        line(t + body + "</" + tag + ">");
    }
    void entity(const Entity& e) {
        line("<Item type=\"CEntityDef\">"); depth++;
        text("archetypeName", hname(e.archetype_name));
        value("flags", std::to_string(e.flags)); value("guid", std::to_string(e.guid));
        vec3("position", e.position); vec4("rotation", e.rotation);
        value("scaleXY", num(e.scale_xy)); value("scaleZ", num(e.scale_z));
        value("parentIndex", std::to_string(e.parent_index)); value("lodDist", num(e.lod_dist)); value("childLodDist", num(e.child_lod_dist));
        text("lodLevel", lod_level(e.lod_level)); value("numChildren", std::to_string(e.num_children)); text("priorityLevel", priority(e.priority_level));
        line(e.extensions.empty() ? "<extensions />" : "<extensions /> <!-- " + std::to_string(e.extensions.size()) + " extensions: see the .ytyp -->");
        value("ambientOcclusionMultiplier", std::to_string(e.ambient_occlusion_multiplier));
        value("artificialAmbientOcclusion", std::to_string(e.artificial_ambient_occlusion));
        value("tintValue", std::to_string(e.tint_value));
        depth--; line("</Item>");
    }
    void archetype(const Archetype& a) {
        const char* type = a.kind == Archetype::Kind::Mlo ? "CMloArchetypeDef" : a.kind == Archetype::Kind::Time ? "CTimeArchetypeDef" : "CBaseArchetypeDef";
        line(std::string("<Item type=\"") + type + "\">"); depth++;
        value("lodDist", num(a.lod_dist)); value("flags", std::to_string(a.flags)); value("specialAttribute", std::to_string(a.special_attribute));
        vec3("bbMin", a.bb_min); vec3("bbMax", a.bb_max); vec3("bsCentre", a.bs_centre);
        value("bsRadius", num(a.bs_radius)); value("hdTextureDist", num(a.hd_texture_dist));
        text("name", hname(a.name)); text("textureDictionary", hname(a.texture_dictionary)); text("clipDictionary", hname(a.clip_dictionary));
        text("drawableDictionary", hname(a.drawable_dictionary)); text("physicsDictionary", hname(a.physics_dictionary));
        text("assetType", asset_type(a.asset_type)); text("assetName", hname(a.asset_name));
        line(a.extensions.empty() ? "<extensions />" : "<extensions /> <!-- " + std::to_string(a.extensions.size()) + " extensions: see the .ytyp -->");
        if (a.kind == Archetype::Kind::Time) value("timeFlags", std::to_string(a.time_flags));
        if (a.kind == Archetype::Kind::Mlo) {
            value("mloFlags", std::to_string(a.mlo_flags));
            line(a.entities.empty() ? "<entities />" : "<entities>");
            if (!a.entities.empty()) { depth++; for (auto& e : a.entities) entity(e); depth--; line("</entities>"); }
            line("<rooms itemType=\"CMloRoomDef\">"); depth++;
            for (auto& r : a.rooms) {
                line("<Item>"); depth++;
                text("name", esc(r.name)); vec3("bbMin", r.bb_min); vec3("bbMax", r.bb_max); value("blend", num(r.blend));
                text("timecycleName", hname(r.timecycle_name)); text("secondaryTimecycleName", hname(r.secondary_timecycle_name));
                value("flags", std::to_string(r.flags)); value("portalCount", std::to_string(r.portal_count)); value("floorId", std::to_string(r.floor_id));
                value("exteriorVisibiltyDepth", std::to_string(r.exterior_visibility_depth));
                uints("attachedObjects", r.attached_objects);
                depth--; line("</Item>");
            }
            depth--; line("</rooms>");
            line("<portals itemType=\"CMloPortalDef\">"); depth++;
            for (auto& p : a.portals) {
                line("<Item>"); depth++;
                value("roomFrom", std::to_string(p.room_from)); value("roomTo", std::to_string(p.room_to)); value("flags", std::to_string(p.flags));
                value("mirrorPriority", std::to_string(p.mirror_priority)); value("opacity", std::to_string(p.opacity)); value("audioOcclusion", std::to_string(p.audio_occlusion));
                line("<corners>"); depth++;
                for (auto& v : p.corners) line("<Item>" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + ", 0</Item>");
                depth--; line("</corners>");
                uints("attachedObjects", p.attached_objects);
                depth--; line("</Item>");
            }
            depth--; line("</portals>");
            line("<entitySets itemType=\"CMloEntitySet\">"); depth++;
            for (auto& s : a.entity_sets) {
                line("<Item>"); depth++;
                text("name", hname(s.name)); uints("locations", s.locations);
                line("<entities>"); depth++; for (auto& e : s.entities) entity(e); depth--; line("</entities>");
                depth--; line("</Item>");
            }
            depth--; line("</entitySets>");
            line("<timeCycleModifiers itemType=\"CMloTimeCycleModifier\">"); depth++;
            for (auto& m : a.time_cycle_modifiers) {
                line("<Item>"); depth++;
                text("name", hname(m.name)); vec4("sphere", m.sphere); value("percentage", num(m.percentage)); value("range", num(m.range));
                value("startHour", std::to_string(m.start_hour)); value("endHour", std::to_string(m.end_hour));
                depth--; line("</Item>");
            }
            depth--; line("</timeCycleModifiers>");
        }
        depth--; line("</Item>");
    }
    std::string ytyp(const MapTypes& t, const Archetype& a, const std::string& name) {
        out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        line("<CMapTypes>"); depth++;
        line("<extensions />");
        line("<archetypes>"); depth++;
        archetype(a);
        depth--; line("</archetypes>");
        text("name", esc(name));
        line("<dependencies />");
        line("<compositeEntityTypes itemType=\"CCompositeEntityType\" />");
        depth--; line("</CMapTypes>");
        return out;
    }
};

bool save(const GameIndex& gi, const IndexedFile& f, const fs::path& dir, std::vector<std::string>& written, const char* what) {
    Bytes data = gi.extract(f);
    fs::path p = dir / gi.file_name(f);
    rsc::write_file(p.string(), data);
    std::printf("  %-22s %s  (%zu bytes, from %s)\n", what, gi.file_name(f).c_str(), data.size(), gi.path(f).c_str());
    written.push_back(p.string());
    return true;
}


// Which textures do the pulled drawables name, and where are they: embedded in the drawable, in a pulled txd, or neither.
struct TextureCheck {
    std::set<std::string> referenced, embedded, in_txd, missing;
};
void collect_textures(const Drawable& d, TextureCheck& t) {
    if (!d.shader_group) return;
    if (d.shader_group->textures) for (auto& x : d.shader_group->textures->textures) t.embedded.insert(lower(x.name));
    for (auto& sh : d.shader_group->shaders)
        for (auto& p : sh.params)
            if (p.type == ParamType::Texture && !p.texture.empty()) t.referenced.insert(lower(p.texture));
}
void check_textures(const GameIndex& gi, const ResolvedDrawable& r, const std::vector<std::string>& txd_files, TextureCheck& t) {
    if (r.type == ResolvedDrawable::None) return;
    Bytes data = gi.extract(*r.file);
    if (r.type == ResolvedDrawable::Ydr) collect_textures(read_ydr(data), t);
    else if (r.type == ResolvedDrawable::Yft) {
        Fragment f = read_yft(data);
        if (f.drawable) collect_textures(*f.drawable, t);
        for (auto& d : f.drawable_array) collect_textures(d, t);
    } else {
        DrawableDictionary dd = read_ydd(data);
        for (auto& d : dd.drawables) collect_textures(d, t);
    }
    for (auto& path : txd_files) {
        TextureDictionary txd = read_ytd(rsc::read_file(path));
        for (auto& x : txd.textures) t.in_txd.insert(lower(x.name));
    }
    for (auto& n : t.referenced)
        if (!t.embedded.count(n) && !t.in_txd.count(n)) t.missing.insert(n);
}

int cmd_pull(const std::vector<std::string>& args) {
    Common c = parse(args);
    if (c.rest.size() != 2) return 2;
    GameIndex gi = open_index(c, "", c.rebuild);
    std::string name = c.rest[0];
    fs::path dir = c.rest[1];
    fs::create_directories(dir);
    std::vector<std::string> written;

    std::string lname = lower(name);
    size_t dot = lname.rfind('.');
    if (dot != std::string::npos && dot > 0 && lname.size() - dot <= 6 && lname.find_first_of("/\\") == std::string::npos) {
        auto f = gi.resolve_file(lname.substr(0, dot), lname.substr(dot));
        if (!f) { std::fprintf(stderr, "error: no file %s in the game\n", name.c_str()); return 1; }
        save(gi, *f, dir, written, "file");
        return 0;
    }

    auto a = gi.archetype(lname);
    if (!a) {
        // not an archetype: every file with this stem
        auto files = gi.resolve_stem(hash_or_name(lname));
        files.erase(std::remove_if(files.begin(), files.end(), [&](const IndexedFile& f) { return gi.extension(f) == ".rpf"; }), files.end());
        if (files.empty()) { std::fprintf(stderr, "error: '%s' is neither an archetype nor a file name in the game\n", name.c_str()); return 1; }
        for (auto& f : files) save(gi, f, dir, written, "file");
        return 0;
    }

    std::printf("archetype %s (%s, %s) from %s [%s]\n", gi.name(a->name).c_str(), kind_name(a->kind), asset_name_of(a->asset_type),
                gi.path(gi.ytyp_of(*a)).c_str(), gi.ranks()[a->rank].label().c_str());
    ResolvedDrawable r = gi.resolve_drawable(*a);
    int missing = 0;
    if (r.type == ResolvedDrawable::None) {
        if (a->asset_type != 4) { std::printf("  (no drawable or fragment found for %s)\n", gi.name(a->name).c_str()); missing++; }
    } else {
        const char* what = r.type == ResolvedDrawable::Ydr ? "drawable" : r.type == ResolvedDrawable::Yft ? "fragment" : "drawable dictionary";
        save(gi, *r.file, dir, written, what);
    }
    // texture dictionary and its parents
    std::set<uint32_t> done;
    std::vector<uint32_t> txds;
    std::vector<std::string> txd_paths;
    if (a->texture_dictionary) txds.push_back(a->texture_dictionary);
    if (a->texture_dictionary) for (uint32_t p : gi.txd_parents(a->texture_dictionary)) txds.push_back(p);
    for (size_t i = 0; i < txds.size(); i++) {
        if (!done.insert(txds[i]).second) continue;
        auto f = gi.resolve_txd(txds[i]);
        if (f) { save(gi, *f, dir, written, i == 0 ? "texture dictionary" : "txd parent"); txd_paths.push_back(written.back()); }
        else { std::printf("  (texture dictionary %s not found)\n", gi.name(txds[i]).c_str()); if (i == 0) missing++; }
    }
    if (a->physics_dictionary) {
        auto f = gi.resolve_file(a->physics_dictionary, ".ybn");
        if (f) save(gi, *f, dir, written, "physics (bounds)");
        else std::printf("  (physics dictionary %s: no .ybn of that name; the bound is inside the drawable or a dictionary)\n", gi.name(a->physics_dictionary).c_str());
    }
    {
        TextureCheck tc;
        check_textures(gi, r, txd_paths, tc);
        std::printf("  textures: %zu named by the shaders, %zu embedded in the model, %zu in the pulled txd(s), %zu in neither%s\n", tc.referenced.size(), tc.embedded.size(), tc.in_txd.size(), tc.missing.size(), tc.missing.empty() ? "" : " (global txds provide them in game):");
        for (auto& m : tc.missing) std::printf("      %s\n", m.c_str());
    }
    // the archetype itself: XML text and a one-archetype .ytyp
    Bytes ytyp_bytes = gi.extract(gi.ytyp_of(*a));
    MapTypes t = read_ytyp(ytyp_bytes);
    const Archetype* src = nullptr;
    for (auto& x : t.archetypes) if (x.name == a->name && x.kind != Archetype::Kind::Other) src = &x;
    if (!src) { std::fprintf(stderr, "error: the archetype vanished from %s\n", gi.path(gi.ytyp_of(*a)).c_str()); return 1; }
    std::string base = gi.name(a->name);
    Xml x{gi};
    std::string xml = x.ytyp(t, *src, base);
    fs::path xp = dir / (base + ".ytyp.xml");
    rsc::write_file(xp.string(), Bytes(xml.begin(), xml.end()));
    std::printf("  %-22s %s\n", "archetype (XML)", xp.filename().string().c_str());
    MapTypes one;
    one.name = joaat_lower(base);
    one.archetypes.push_back(*src);
    one.structs = t.structs;
    one.enums = t.enums;
    fs::path yp = dir / (base + ".ytyp");
    rsc::write_file(yp.string(), write_ytyp(one));
    std::printf("  %-22s %s\n", "archetype (ytyp)", yp.filename().string().c_str());
    return missing ? 3 : 0;
}

} // namespace

// rk shaders build <cache> [--game dir] | rk shaders list <cache> | rk shaders show <cache> <preset>:
// the shader database from the install's .sps / .fxc files, and what instantiate() makes of a preset
int cmd_shaders(const std::vector<std::string>& args) {
    Common c = parse(args);
    if (c.rest.size() < 2) return 2;
    const std::string sub = c.rest[0], cache = c.rest[1];
    if (sub == "build") {
        auto t0 = std::chrono::steady_clock::now();
        GameIndex gi = open_index(c, "", false);
        ShaderDb db = ShaderDb::from_game(gi);
        if (!db.save(cache)) throw Error("cannot write " + cache);
        std::printf("%zu base shaders, %zu presets in %.1f s -> %s\n", db.base_count(), db.preset_count(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), cache.c_str());
        return 0;
    }
    ShaderDb db = ShaderDb::load(cache);
    if (db.empty()) throw Error("no shader database in " + cache + " (rk shaders build first)");
    if (sub == "list") {
        for (auto& n : db.preset_names()) { const ShaderPresetDef* p = db.preset(n); std::printf("%-36s base %-28s bucket %d\n", n.c_str(), p->base.c_str(), p->bucket); }
        return 0;
    }
    if (sub == "show" && c.rest.size() >= 3) {
        const ShaderPresetDef* p = db.preset(c.rest[2]);
        if (!p) { std::printf("no preset %s\n", c.rest[2].c_str()); return 1; }
        const ShaderBaseDef* b = db.base(p->base);
        std::printf("%s: base %s, bucket %d, vertex type %u\n", p->name.c_str(), p->base.c_str(), p->bucket, b ? b->vertex_type : 0);
        Shader s = db.instantiate(*p);
        for (const ShaderParam& prm : s.params) {
            std::string n = "?";
            if (b) for (auto& d : b->textures) if (joaat_lower(d.name) == prm.name_hash) n = d.name;
            if (b) for (auto& d : b->vectors) if (joaat_lower(d.name) == prm.name_hash) n = d.name;
            if (prm.type == ParamType::Texture) std::printf("  %-28s texture @%u\n", n.c_str(), prm.slot);
            else std::printf("  %-28s vec @%u (%g %g %g %g)%s\n", n.c_str(), prm.slot, prm.values[0].x, prm.values[0].y, prm.values[0].z, prm.values[0].w,
                             prm.values.size() > 1 ? (" x" + std::to_string(prm.values.size())).c_str() : "");
        }
        return 0;
    }
    return 2;
}

// rk world info | at <x> <y> <z> | near <x> <y> <z> <radius> | mlos [substring]: the world index (include/ragekit/world.h)
int cmd_world(const std::vector<std::string>& args) {
    Common c = parse(args);
    bool all_lods = false, groups = false, sp = false;
    std::vector<std::string> rest;
    for (auto& a : c.rest) {
        if (a == "--all-lods") all_lods = true;
        else if (a == "--groups") groups = true;
        else if (a == "--sp") sp = true;
        else rest.push_back(a);
    }
    if (rest.empty()) return 2;
    const std::string sub = rest[0];
    GameIndex gi = open_index(c, "", false);
    WorldIndexOptions o;
    o.rebuild = c.rebuild;
    o.sp_map = sp;
    o.progress = [](const std::string& s) { std::fprintf(stderr, "%s\n", s.c_str()); };
    WorldIndex w = WorldIndex::open(gi, o);
    WorldQuery q;
    if (all_lods) q.lod_mask = 0x7F;
    q.groups = groups;
    auto f = [&](size_t i) { return (float)std::atof(rest.at(i).c_str()); };
    auto print = [&](const std::vector<WorldEntity>& es) {
        for (auto& e : es)
            std::printf("%-36s %s lod %d  pos %.2f %.2f %.2f  box %.1f %.1f %.1f .. %.1f %.1f %.1f  %s\n", gi.name(e.def.archetype_name).c_str(), e.def.is_mlo_instance ? "MLO" : "   ",
                        e.def.lod_level, e.def.position.x, e.def.position.y, e.def.position.z, e.box_min.x, e.box_min.y, e.box_min.z, e.box_max.x, e.box_max.y, e.box_max.z,
                        gi.name(w.maps()[e.map].name).c_str());
    };
    if (sub == "info") {
        const auto& in = w.info();
        size_t grouped = 0, ents = 0, interior = 0;
        for (auto& m : w.maps()) { grouped += m.group; ents += m.entity_count; }
        for (auto& b : w.bounds()) interior += b.interior != 0;
        std::printf("%s in %.2f s (scan %.1f s): %s\n", in.from_cache ? "cache" : "scanned", in.seconds, in.scan_seconds, in.cache_path.c_str());
        std::printf("maps %zu (IPL groups %zu), entities %zu, MLO instances %zu, collision files %zu (interior %zu)\n", w.maps().size(), grouped, ents,
                    w.mlo_instances().size(), w.bounds().size(), interior);
        std::printf("map change sets: %u archives off, %u on; warnings %zu\n", in.archives_invalidated, in.archives_enabled, in.warnings.size());
        for (auto& s : in.warnings) std::printf("  %s\n", s.c_str());
        return 0;
    }
    if (sub == "at" && rest.size() >= 4) { print(w.entities_at({f(1), f(2), f(3)}, q)); return 0; }
    if (sub == "near" && rest.size() >= 5) {
        const Vec3 p{f(1), f(2), f(3)}, r{f(4), f(4), f(4)};
        print(w.entities_in(p - r, p + r, q));
        for (uint32_t b : w.bounds_in(p - r, p + r))
            std::printf("collision %-28s layer %u box %.1f %.1f %.1f .. %.1f %.1f %.1f\n", gi.file_name(w.bounds()[b].file).c_str(), w.bounds()[b].layer & 0xFF,
                        w.bounds()[b].box_min.x, w.bounds()[b].box_min.y, w.bounds()[b].box_min.z, w.bounds()[b].box_max.x, w.bounds()[b].box_max.y, w.bounds()[b].box_max.z);
        return 0;
    }
    if (sub == "occluders" && rest.size() >= 5) {   // the box occluders and occlude models within a radius of a point, per ymap
        const Vec3 p{f(1), f(2), f(3)}, r{f(4), f(4), f(4)};
        OrientedBox area;
        area.centre = p;
        area.half = r;
        size_t maps_read = 0, maps_with = 0;
        for (size_t i = 0; i < w.maps().size(); i++) {
            const WorldMap& m = w.maps()[i];
            if (!m.readable || m.streaming_max.x < p.x - r.x || m.streaming_min.x > p.x + r.x || m.streaming_max.y < p.y - r.y || m.streaming_min.y > p.y + r.y) continue;
            std::shared_ptr<const MapData> d;
            try { d = w.map_data((uint32_t)i); } catch (const std::exception&) { continue; }
            maps_read++;
            if (d->box_occluders.empty() && d->occlude_models.empty()) continue;
            maps_with++;
            const OccluderHits hits = find_occluders_in(*d, area);
            std::printf("%s  content %u flags %u parent %s  streaming %.0f %.0f %.0f .. %.0f %.0f %.0f  entities %zu  box occluders %zu (%zu near) occlude models %zu (%zu near)\n",
                        gi.name(m.name).c_str(), d->content_flags, d->flags, d->parent ? gi.name(d->parent).c_str() : "-", m.streaming_min.x, m.streaming_min.y, m.streaming_min.z,
                        m.streaming_max.x, m.streaming_max.y, m.streaming_max.z, d->entities.size(), d->box_occluders.size(), hits.boxes.size(), d->occlude_models.size(), hits.models.size());
            for (uint32_t k : hits.boxes) {
                const BoxOccluder& b = d->box_occluders[k];
                const OccluderBox o = decode_box_occluder(b);
                std::printf("    box %u: centre %.2f %.2f %.2f  size %.2f %.2f %.2f  dir %.3f %.3f   raw cx %d cy %d cz %d cos %d len %d wid %d hei %d sin %d\n", k, o.centre.x, o.centre.y,
                            o.centre.z, o.size.x, o.size.y, o.size.z, o.dir_x, o.dir_y, b.center_x, b.center_y, b.center_z, b.cos_z, b.length, b.width, b.height, b.sin_z);
            }
            for (uint32_t k : hits.models) {
                const OccludeMesh o = read_occlude_model(d->occlude_models[k]);
                std::printf("    model %u: %zu vertices %zu triangles  bounds %.2f %.2f %.2f .. %.2f %.2f %.2f  flags %u\n", k, o.vertices.size(), o.triangles(), o.bmin.x, o.bmin.y,
                            o.bmin.z, o.bmax.x, o.bmax.y, o.bmax.z, o.flags);
            }
        }
        std::printf("%zu ymaps read, %zu with occluders\n", maps_read, maps_with);
        return 0;
    }
    if (sub == "mlos") {
        const std::string want = rest.size() > 1 ? lower(rest[1]) : "";
        for (auto& m : w.mlo_instances()) {
            const std::string n = gi.name(m.archetype);
            if (!want.empty() && lower(n).find(want) == std::string::npos) continue;
            std::printf("%-32s pos %.2f %.2f %.2f  yaw %.1f  %s\n", n.c_str(), m.position.x, m.position.y, m.position.z,
                        2.0 * std::atan2(m.orientation.z, m.orientation.w) * 57.29578, gi.name(w.maps()[m.map].name).c_str());
        }
        return 0;
    }
    return 2;
}

// rk navmesh cell <x> <y> | check <ynv|cell> | patch <x0,y0,x1,y1> -o <out.ynv> [...]: navmeshes (navmesh.h, navbuild.h)
std::vector<float> floats(const std::string& s) {
    std::vector<float> v;
    size_t i = 0;
    while (i <= s.size()) {
        const size_t j = std::min(s.find(',', i), s.size());
        v.push_back((float)std::atof(s.substr(i, j - i).c_str()));
        i = j + 1;
    }
    return v;
}
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i < s.size()) {
        const size_t j = std::min(s.find(',', i), s.size());
        if (j > i) v.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return v;
}

int cmd_navmesh(const std::vector<std::string>& args) {
    Common c = parse(args);
    std::vector<std::string> rest, ybns, ymaps, props, exclude;
    std::string out, in;
    std::optional<std::vector<float>> zr, seed;
    bool game_props = false, sp = false;
    float prop_radius = 0.12f;
    for (size_t i = 0; i < c.rest.size(); i++) {
        const std::string& a = c.rest[i];
        auto next = [&]() -> const std::string& { if (i + 1 >= c.rest.size()) throw Error(a + " takes a value"); return c.rest[++i]; };
        if (a == "-o") out = next();
        else if (a == "--in") in = next();
        else if (a == "--z") zr = floats(next());
        else if (a == "--seed") seed = floats(next());
        else if (a == "--ybn") ybns.push_back(next());
        else if (a == "--ymap") ymaps.push_back(next());
        else if (a == "--props") for (auto& w : words(next())) props.push_back(w);
        else if (a == "--prop-radius") prop_radius = (float)std::atof(next().c_str());
        else if (a == "--exclude") for (auto& w : words(next())) exclude.push_back(lower(w));
        else if (a == "--game-props") game_props = true;
        else if (a == "--sp") sp = true;
        else rest.push_back(a);
    }
    if (rest.empty()) return 2;
    const std::string sub = rest[0];
    if (sub == "cell" && rest.size() == 3) {
        const NavCell cell = NavCell::of((float)std::atof(rest[1].c_str()), (float)std::atof(rest[2].c_str()));
        GameIndex gi = open_index(c, "", false);
        const std::string stem = cell.file_name().substr(0, cell.file_name().size() - 4);
        const auto f = gi.resolve_file(stem, ".ynv");
        std::printf("%s  area %u  x %.0f..%.0f y %.0f..%.0f  %s\n", cell.file_name().c_str(), cell.area_id(), cell.min().x, cell.max().x, cell.min().y, cell.max().y,
                    f ? gi.path(*f).c_str() : "(not in the game)");
        return 0;
    }
    auto load = [&](const std::string& name, std::string* where) {
        if (fs::exists(name)) { if (where) *where = name; return read_ynv(rsc::read_file(name)); }
        GameIndex gi = open_index(c, "", false);
        std::string stem = name;
        if (stem.size() > 4 && lower(stem.substr(stem.size() - 4)) == ".ynv") stem.resize(stem.size() - 4);
        const auto f = gi.resolve_file(stem, ".ynv");
        if (!f) throw Error("no " + name + " (a file, or a navmesh of the game)");
        if (where) *where = gi.path(*f);
        return read_ynv(gi.extract(*f));
    };
    if (sub == "check" && rest.size() == 2) {
        std::string where;
        const NavMesh n = load(rest[1], &where);
        const auto bad = n.check_links();
        std::printf("%s: area %u, %zu polygons, %zu cover points; %zu links that do not hold\n", where.c_str(), n.area_id, n.polys.size(), n.points().size(), bad.size());
        for (size_t i = 0; i < bad.size() && i < 20; i++) std::printf("  %s\n", bad[i].c_str());
        return bad.empty() ? 0 : 1;
    }
    if (sub != "patch" || rest.size() != 2) return 2;
    if (out.empty()) throw Error("rk navmesh patch: give the output file with -o");
    const auto rect = floats(rest[1]);
    if (rect.size() != 4) throw Error("rk navmesh patch: the area is x0,y0,x1,y1");
    NavPatchOptions o;
    o.x0 = std::min(rect[0], rect[2]); o.x1 = std::max(rect[0], rect[2]);
    o.y0 = std::min(rect[1], rect[3]); o.y1 = std::max(rect[1], rect[3]);
    if (zr) { if (zr->size() != 2) throw Error("--z takes lo,hi"); o.z_lo = (*zr)[0]; o.z_hi = (*zr)[1]; }
    if (seed) { if (seed->size() != 3) throw Error("--seed takes x,y,z"); o.seed = Vec3{(*seed)[0], (*seed)[1], (*seed)[2]}; }
    const NavCell cell = NavCell::of(o.x0, o.y0), cell1 = NavCell::of(o.x1 - 1e-3f, o.y1 - 1e-3f);
    if (cell.x != cell1.x || cell.y != cell1.y)
        throw Error("the area spans two navmesh cells (" + cell.file_name() + ", " + cell1.file_name() + "): split it at the 150 m border");

    GameIndex gi = open_index(c, "", false);
    std::string where;
    NavMesh nav;
    if (!in.empty()) { nav = read_ynv(rsc::read_file(in)); where = in; }
    else {
        const auto f = gi.resolve_file(cell.file_name().substr(0, cell.file_name().size() - 4), ".ynv");
        if (!f) throw Error("the game has no " + cell.file_name());
        nav = read_ynv(gi.extract(*f));
        where = gi.path(*f);
    }
    if (nav.area_id != cell.area_id()) throw Error(where + " is area " + std::to_string(nav.area_id) + ", the area given lies in " + std::to_string(cell.area_id()));
    const NavMesh before = nav;

    // collision: the given files, and the game's static collision reaching the area where no given file replaces it
    const AABB ext = nav_patch_extent(nav, o);
    NavCollision col;
    std::set<std::string> given;
    auto skip = [&](const std::string& name) {
        const std::string n = lower(name), stem = n.size() > 4 && n.substr(n.size() - 4) == ".ybn" ? n.substr(0, n.size() - 4) : n;
        return std::find(exclude.begin(), exclude.end(), n) != exclude.end() || std::find(exclude.begin(), exclude.end(), stem) != exclude.end();
    };
    for (const std::string& p : ybns) {
        const std::string name = lower(fs::path(p).filename().string());
        if (skip(name)) continue;
        given.insert(name);
        const BoundPtr b = read_ybn(rsc::read_file(p));
        const size_t was = col.triangles.size();
        col.add_bound(*b, &ext);
        if (col.triangles.size() > was) std::printf("collision %s (given): %zu triangles\n", name.c_str(), col.triangles.size() - was);
    }
    WorldIndexOptions wo;
    wo.sp_map = sp;
    wo.progress = [](const std::string& s) { std::fprintf(stderr, "%s\n", s.c_str()); };
    WorldIndex w = WorldIndex::open(gi, wo);
    const Vec3 emn = ext.min.xyz(), emx = ext.max.xyz();
    for (uint32_t bi : w.bounds_in(emn, emx)) {
        const WorldBounds& wb = w.bounds()[bi];
        const std::string name = gi.file_name(wb.file);
        if ((wb.layer & 0xFF) == 1 || (wb.layer & 0xFF) == 2 || given.count(name) || skip(name)) continue;   // hi@ (vehicles only), ma@
        const BoundPtr b = read_ybn(gi.extract(wb.file));
        const size_t was = col.triangles.size();
        col.add_bound(*b, &ext);
        if (col.triangles.size() > was) std::printf("collision %s (%s): %zu triangles\n", name.c_str(), gi.archive_path(wb.file).c_str(), col.triangles.size() - was);
    }
    // posts: the named props placed by the given ymaps (and the game's, with --game-props)
    if (!props.empty()) {
        std::set<uint32_t> want;
        for (auto& p : props) want.insert(joaat_lower(p));
        auto add = [&](const Entity& e) {
            const Vec3 q = e.position;
            if (!want.count(e.archetype_name) || q.x < emn.x || q.x > emx.x || q.y < emn.y || q.y > emx.y || q.z < emn.z || q.z > emx.z) return;
            col.posts.push_back({q.x, q.y, q.z, prop_radius, 1.0f});
        };
        for (const std::string& p : ymaps) for (const Entity& e : read_ymap(rsc::read_file(p)).entities) add(e);
        if (game_props) for (const WorldEntity& e : w.entities_in(emn, emx)) add(e.def);
        std::printf("posts: %zu props cut in\n", col.posts.size());
    }

    const NavPatchResult r = patch_navmesh(nav, o, col);
    const Bytes written = write_ynv(nav);
    rsc::write_file(out, written);
    const NavMesh back = read_ynv(written);
    const auto bad = back.check_links(r.written);
    const auto cross = cross_cell_changes(before, back);
    std::printf("%s (from %s): %u polygons replaced by %u (%u height regions), %u kept polygons stitched, %u links, %u open sides, %u samples blocked by "
                "rails, posts and props, %u cover points dropped, worst polygon %.2f m off the ground\n",
                cell.file_name().c_str(), where.c_str(), r.removed, r.created, r.regions, r.kept_changed, r.links, r.open_sides, r.blocked_samples, r.points_dropped,
                r.worst_height);
    for (auto& n : r.notes) std::printf("  note: %s\n", n.c_str());
    for (size_t i = 0; i < bad.size() && i < 20; i++) std::printf("  link: %s\n", bad[i].c_str());
    for (auto& x : cross) std::printf("  link into the next cell changed: %s\n", x.c_str());
    std::printf("%s: %zu polygons, %zu bytes; %s\n", out.c_str(), back.polys.size(), written.size(),
                bad.empty() && cross.empty() ? "every link checked" : "LINKS DO NOT HOLD");
    return bad.empty() && cross.empty() ? 0 : 1;
}

int cmd_gameindex(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "navmesh") return cmd_navmesh(args);
    if (cmd == "world") return cmd_world(args);
    if (cmd == "shaders") return cmd_shaders(args);
    if (cmd == "index") return cmd_index(args);
    if (cmd == "find") return cmd_find(args);
    if (cmd == "pull") return cmd_pull(args);
    return 2;
}
