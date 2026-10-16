// rk_gen9_test: ragekit's conversions between GTA V Legacy and Enhanced, held against the game's own: every drawable file
// (.ydr .ydd .yft) and texture dictionary (.ytd) found at the same path in a Legacy and an Enhanced install is read from
// both. The Enhanced file converted to Legacy (read, write_* Legacy, read) is compared with the Legacy file, and the Legacy
// file converted to Enhanced with the Enhanced file. Rockstar made the Enhanced files from the Legacy ones, so where the two
// editions hold the same asset, what ragekit writes should hold what the game's own file holds.
//
//     rk_gen9_test <legacy install> <enhanced install> [--every=N] [--only=.ydr,.ytd] [--verbose]
//
// Prints, per kind of difference, how many files and items show it, with an example; then the totals.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "ragekit/ragekit.h"
#include "shader_gen9.h"

using namespace rk;

namespace {

std::string lower_copy(std::string s) { for (auto& c : s) c = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); return s; }

struct Stats {
    struct Kind { size_t items = 0, files = 0; std::string example; };
    std::map<std::string, Kind> kinds;
    std::map<std::string, size_t> counted;   // items compared per category
    std::string file;
    std::map<std::string, bool> in_file;
    bool verbose = false;
    void diff(const std::string& kind, const std::string& what) {
        Kind& k = kinds[kind];
        k.items++;
        if (!in_file[kind]) { k.files++; in_file[kind] = true; }
        if (k.example.empty()) k.example = file + ": " + what;
        if (verbose) std::printf("  %s: %s: %s\n", file.c_str(), kind.c_str(), what.c_str());
    }
    void count(const std::string& what, size_t n = 1) { counted[what] += n; }
    void begin(const std::string& f) { file = f; in_file.clear(); }
};

bool feq(float a, float b) { return std::memcmp(&a, &b, 4) == 0 || std::fabs(a - b) <= 1e-6f * std::max(1.f, std::fabs(a)); }
bool veq(const Vec4& a, const Vec4& b) { return feq(a.x, b.x) && feq(a.y, b.y) && feq(a.z, b.z) && feq(a.w, b.w); }

std::string vstr(const Vec4& v) { char b[96]; std::snprintf(b, sizeof b, "(%g %g %g %g)", v.x, v.y, v.z, v.w); return b; }

// The Legacy view of a shader converted from Enhanced (`got`) against the game's Legacy shader (`want`).
void compare_legacy_shader(Stats& st, const Shader& got, const Shader& want, const std::string& where) {
    st.count("shaders");
    if (got.name_hash != want.name_hash) { st.diff("shader name", where); return; }
    if (got.render_bucket != want.render_bucket) st.diff("shader bucket", where);
    if (got.sps_hash != want.sps_hash) st.diff("shader sps", where + " " + hash_name(got.sps_hash) + " vs " + hash_name(want.sps_hash));
    if (got.unk12 != want.unk12) st.diff("shader unk12", where);
    std::vector<uint32_t> gn, wn;
    for (auto& p : got.params) gn.push_back(p.name_hash);
    for (auto& p : want.params) wn.push_back(p.name_hash);
    if (gn != wn) {
        std::string d = where + " " + hash_name(got.name_hash) + ": got";
        for (auto h : gn) d += " " + hash_name(h);
        d += " / want";
        for (auto h : wn) d += " " + hash_name(h);
        st.diff("shader parameter names / order", d);
        return;
    }
    for (size_t i = 0; i < got.params.size(); i++) {
        const auto& a = got.params[i];
        const auto& b = want.params[i];
        st.count("parameters");
        const std::string pw = where + " " + hash_name(got.name_hash) + "." + hash_name(a.name_hash);
        if (a.slot != b.slot) st.diff("parameter slot", pw);
        if (a.type != b.type) { st.diff("parameter type", pw); continue; }
        if (a.type == ParamType::Texture) {
            if (lower_copy(a.texture) != lower_copy(b.texture)) st.diff("parameter texture", pw + " '" + a.texture + "' vs '" + b.texture + "'");
        } else {
            if (a.values.size() != b.values.size()) { st.diff("parameter value count", pw); continue; }
            for (size_t k = 0; k < a.values.size(); k++)
                if (!veq(a.values[k], b.values[k])) { st.diff("parameter value", pw + " " + vstr(a.values[k]) + " vs " + vstr(b.values[k])); break; }
        }
    }
}

// An Enhanced shader written from a Legacy one (`got`) against the game's Enhanced shader (`want`).
void compare_gen9_shader(Stats& st, const Shader& got, const Shader& want, const std::string& where) {
    st.count("shaders");
    if (!got.gen9 || !want.gen9) { st.diff("shader without Enhanced data", where); return; }
    const auto& a = *got.gen9;
    const auto& b = *want.gen9;
    const std::string w = where + " " + hash_name(got.name_hash);
    if (got.render_bucket != want.render_bucket) st.diff("shader bucket", w);
    bool same_table = a.params.size() == b.params.size();
    for (size_t i = 0; same_table && i < a.params.size(); i++) same_table = a.params[i].name_hash == b.params[i].name_hash && a.params[i].data == b.params[i].data;
    if (!same_table || a.buffer_sizes != b.buffer_sizes) {
        st.diff("Enhanced layout (another build of the effect)", w);
        // compare by name what both have
    }
    for (auto& p : b.params) {
        const ShaderParamGen9* q = a.find(p.name_hash);
        st.count("parameters");
        if (!q) { st.diff("Enhanced parameter missing", w + "." + hash_name(p.name_hash)); continue; }
        if (p.kind() == ShaderParamGen9::Texture) {
            const std::string ta = q->slot() < a.textures.size() ? a.textures[q->slot()] : "";
            const std::string tb = p.slot() < b.textures.size() ? b.textures[p.slot()] : "";
            if (lower_copy(ta) != lower_copy(tb)) st.diff("Enhanced texture", w + "." + hash_name(p.name_hash) + " '" + ta + "' vs '" + tb + "'");
        } else if (p.kind() == ShaderParamGen9::Constant) {
            const size_t oa = a.buffer_offset(q->slot()) + q->offset(), ob = b.buffer_offset(p.slot()) + p.offset();
            const size_t n = std::min(q->length(), p.length());
            if (oa + n > a.buffers.size() || ob + n > b.buffers.size()) { st.diff("Enhanced constant out of range", w); continue; }
            bool same = true;
            for (size_t k = 0; k + 4 <= n; k += 4) {
                float x, y;
                std::memcpy(&x, a.buffers.data() + oa + k, 4);
                std::memcpy(&y, b.buffers.data() + ob + k, 4);
                if (!feq(x, y)) same = false;
            }
            if (!same) {
                float x[4] = {}, y[4] = {};
                std::memcpy(x, a.buffers.data() + oa, std::min<size_t>(n, 16));
                std::memcpy(y, b.buffers.data() + ob, std::min<size_t>(n, 16));
                const bool legacy_has = shader_table::legacy_name(got.name_hash, p.name_hash) != 0;
                st.diff(legacy_has ? "Enhanced constant (from a Legacy parameter)" : "Enhanced constant (Enhanced only: the table's value)",
                        w + "." + hash_name(p.name_hash) + " " + vstr({x[0], x[1], x[2], x[3]}) + " vs " + vstr({y[0], y[1], y[2], y[3]}));
            }
        }
    }
}

void compare_geometry(Stats& st, const Geometry& a, const Geometry& b, const std::string& w) {
    st.count("geometries");
    if (a.vertex_count != b.vertex_count) { st.diff("geometry vertex count", w); return; }
    if (a.indices != b.indices) st.diff("geometry indices", w);
    if (a.layout.flags != b.layout.flags) { st.diff("geometry semantics", w); return; }
    if (a.layout.types != b.layout.types) st.diff("geometry layout types", w + " " + hash_name((uint32_t)a.layout.types) + " vs " + hash_name((uint32_t)b.layout.types));
    if (a.layout.types == b.layout.types && a.vertices != b.vertices) {
        // the same layout: compare the components
        for (int s = 0; s < 16; s++) {
            if (!a.layout.has((VertexSemantic)s)) continue;
            for (uint32_t v = 0; v < a.vertex_count; v++)
                if (!veq(a.get(v, (VertexSemantic)s), b.get(v, (VertexSemantic)s))) { st.diff("geometry vertex component " + std::to_string(s), w); break; }
        }
    } else if (a.layout.types != b.layout.types) {
        for (int s = 0; s < 16; s++) {
            if (!a.layout.has((VertexSemantic)s)) continue;
            for (uint32_t v = 0; v < a.vertex_count; v++) {
                const Vec4 x = a.get(v, (VertexSemantic)s), y = b.get(v, (VertexSemantic)s);
                if (std::fabs(x.x - y.x) > 0.01f || std::fabs(x.y - y.y) > 0.01f || std::fabs(x.z - y.z) > 0.01f || std::fabs(x.w - y.w) > 0.01f) {
                    st.diff("geometry vertex component " + std::to_string(s) + " (converted type)", w);
                    break;
                }
            }
        }
    }
    if (a.bone_ids != b.bone_ids) st.diff("geometry bone ids", w);
    if (a.vb_flags != b.vb_flags) st.diff("geometry vb_flags", w + " " + std::to_string(a.vb_flags) + " vs " + std::to_string(b.vb_flags));
}

void compare_drawables(Stats& st, const Drawable& got, const Drawable& want, bool to_gen9, const std::string& w) {
    st.count("drawables");
    if (got.shader_group.has_value() != want.shader_group.has_value()) { st.diff("shader group presence", w); return; }
    if (got.shader_group) {
        const auto& a = got.shader_group->shaders;
        const auto& b = want.shader_group->shaders;
        if (a.size() != b.size()) st.diff("shader count", w);
        for (size_t i = 0; i < a.size() && i < b.size(); i++) {
            if (to_gen9) compare_gen9_shader(st, a[i], b[i], w + " shader " + std::to_string(i));
            else compare_legacy_shader(st, a[i], b[i], w + " shader " + std::to_string(i));
        }
        const bool ta = got.shader_group->textures.has_value(), tb = want.shader_group->textures.has_value();
        if (ta != tb) st.diff("embedded textures presence", w);
    }
    for (int l = 0; l < 4; l++) {
        if (got.lods[l].size() != want.lods[l].size()) { st.diff("model count", w); continue; }
        for (size_t m = 0; m < got.lods[l].size(); m++) {
            const auto& x = got.lods[l][m];
            const auto& y = want.lods[l][m];
            if (x.geometries.size() != y.geometries.size()) { st.diff("geometry count", w); continue; }
            for (size_t g = 0; g < x.geometries.size(); g++)
                compare_geometry(st, x.geometries[g], y.geometries[g], w + " lod" + std::to_string(l) + " model " + std::to_string(m) + " geom " + std::to_string(g));
        }
    }
    if (got.skeleton.has_value() != want.skeleton.has_value()) st.diff("skeleton presence", w);
    else if (got.skeleton && got.skeleton->bones.size() != want.skeleton->bones.size()) st.diff("bone count", w);
    if (got.lights.size() != want.lights.size()) st.diff("light count", w);
}

void compare_textures(Stats& st, const TextureDictionary& got, const TextureDictionary& want, const std::string& w) {
    if (got.textures.size() != want.textures.size()) { st.diff("texture count", w); return; }
    for (size_t i = 0; i < got.textures.size(); i++) {
        const Texture& a = got.textures[i];
        const Texture& b = want.textures[i];
        st.count("textures");
        const std::string tw = w + " " + a.name;
        if (a.name != b.name) { st.diff("texture name", tw); continue; }
        if (a.width != b.width || a.height != b.height || a.levels != b.levels || a.format != b.format) { st.diff("texture image (another image)", tw); continue; }
        if (a.data != b.data) st.diff("texture data", tw);
        if (a.usage_data != b.usage_data) { char u[64]; std::snprintf(u, sizeof u, " %08X vs %08X", a.usage_data, b.usage_data); st.diff("texture usage", tw + u); }
        if (a.gen9 && b.gen9) {
            if (a.gen9->unk23 != b.gen9->unk23) st.diff("texture gen9 unk23", tw);
            if (a.gen9->flags != b.gen9->flags) st.diff("texture gen9 flags", tw);
        }
        if (!a.gen9 && !b.gen9) {
            if (a.unk32 != b.unk32) st.diff("texture unk32", tw + " " + std::to_string(a.unk32) + " vs " + std::to_string(b.unk32));
            if (a.extra_flags != b.extra_flags) st.diff("texture extra_flags", tw);
        }
    }
}

std::vector<const Drawable*> drawables_of(const Fragment& f) {
    std::vector<const Drawable*> out;
    if (f.drawable) out.push_back(&*f.drawable);
    if (f.drawable_cloth) out.push_back(&*f.drawable_cloth);
    for (auto& d : f.drawable_array) out.push_back(&d);
    for (auto& lod : f.physics)
        if (lod) for (auto& c : lod->children) { if (c.drawable) out.push_back(&*c.drawable); if (c.drawable_damaged) out.push_back(&*c.drawable_damaged); }
    return out;
}

/// `got` (converted to `game`) against `want` (the game's own file of that edition).
void compare_file(Stats& st, const std::string& ext, const Bytes& got_file, const Bytes& want_file, Game game, const std::string& dir) {
    const bool to_gen9 = game == Game::Enhanced;
    if (ext == ".ytd") {
        compare_textures(st, read_ytd(got_file), read_ytd(want_file), dir);
    } else if (ext == ".ydr") {
        compare_drawables(st, read_ydr(got_file), read_ydr(want_file), to_gen9, dir);
    } else if (ext == ".ydd") {
        const DrawableDictionary a = read_ydd(got_file), b = read_ydd(want_file);
        if (a.hashes != b.hashes) { st.diff("dictionary names", dir); return; }
        for (size_t i = 0; i < a.drawables.size(); i++) compare_drawables(st, a.drawables[i], b.drawables[i], to_gen9, dir + " " + hash_name(a.hashes[i]));
    } else if (ext == ".yft") {
        const Fragment a = read_yft(got_file), b = read_yft(want_file);
        const auto da = drawables_of(a), db = drawables_of(b);
        if (da.size() != db.size()) { st.diff("fragment drawable count", dir); return; }
        for (size_t i = 0; i < da.size(); i++) compare_drawables(st, *da[i], *db[i], to_gen9, dir + " drawable " + std::to_string(i));
        if (a.glass_windows != b.glass_windows) st.diff("fragment glass windows", dir);
    }
}

Bytes convert(const std::string& ext, const Bytes& file, Game to) {
    if (ext == ".ytd") return write_ytd(read_ytd(file), to);
    if (ext == ".ydr") return write_ydr(read_ydr(file), to);
    if (ext == ".ydd") return write_ydd(read_ydd(file), to);
    return write_yft(read_yft(file), to);
}

}   // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: rk_gen9_test <legacy install> <enhanced install> [--every=N] [--only=.ydr,.ytd] [--verbose]\n");
        return 2;
    }
    size_t every = 1;
    std::string only;
    Stats to_legacy, to_enhanced;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a.rfind("--every=", 0) == 0) every = std::max<size_t>(1, std::stoul(a.substr(8)));
        else if (a.rfind("--only=", 0) == 0) only = "," + a.substr(7) + ",";
        else if (a == "--verbose") to_legacy.verbose = to_enhanced.verbose = true;
    }
    GameIndex legacy = GameIndex::open(argv[1]);
    GameIndex enhanced = GameIndex::open(argv[2]);
    std::map<std::string, const IndexedFile*> legacy_by_path;
    for (const IndexedFile& f : legacy.files()) legacy_by_path[lower_copy(legacy.path(f))] = &f;
    std::map<std::string, size_t> seen;
    size_t pairs = 0, failed = 0, skipped = 0;
    for (const IndexedFile& f : enhanced.files()) {
        const std::string ext = enhanced.extension(f);
        if (ext != ".ydr" && ext != ".ydd" && ext != ".yft" && ext != ".ytd") continue;
        if (!only.empty() && only.find("," + ext + ",") == std::string::npos) continue;
        if (seen[ext]++ % every) continue;
        auto it = legacy_by_path.find(lower_copy(enhanced.path(f)));
        if (it == legacy_by_path.end()) continue;
        const std::string path = enhanced.path(f);
        try {
            const Bytes e = enhanced.extract(f), l = legacy.extract(*it->second);
            if (rsc::game_of(ext, rsc::Resource::load(e).version) != Game::Enhanced || rsc::game_of(ext, rsc::Resource::load(l).version) != Game::Legacy) { skipped++; continue; }
            pairs++;
            to_legacy.begin(path);
            compare_file(to_legacy, ext, convert(ext, e, Game::Legacy), l, Game::Legacy, path);
            to_enhanced.begin(path);
            compare_file(to_enhanced, ext, convert(ext, l, Game::Enhanced), e, Game::Enhanced, path);
        } catch (const std::exception& ex) {
            failed++;
            std::printf("FAIL  %s: %s\n", path.c_str(), ex.what());
        }
    }
    for (int dir = 0; dir < 2; dir++) {
        const Stats& st = dir == 0 ? to_legacy : to_enhanced;
        std::printf("\n== %s\n", dir == 0 ? "Enhanced converted to Legacy, against the game's Legacy files" : "Legacy converted to Enhanced, against the game's Enhanced files");
        for (auto& [k, n] : st.counted) std::printf("   compared %-12s %zu\n", k.c_str(), n);
        for (auto& [k, v] : st.kinds) std::printf("   %-60s %7zu items %6zu files   e.g. %s\n", k.c_str(), v.items, v.files, v.example.substr(0, 400).c_str());
    }
    std::printf("\n%zu pairs, %zu failed, %zu skipped (not one edition each)\n", pairs, failed, skipped);
    return failed ? 1 : 0;
}
