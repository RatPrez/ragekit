// rk_gen_shader_table: writes src/shader_gen9.inc, the table ragekit converts shaders between GTA V Legacy and Enhanced with.
//
// From a Legacy install: each effect's material parameters (.fxc, common/shaders/win32_40_final: the Legacy names, the
// Enhanced name every variable carries as its second name, the order and slots a drawable stores them in, the defaults),
// and the preset and unk12 the game's drawables use per shader and draw bucket. From an Enhanced install: each shader's
// parameter layout as the game's drawables carry it (the table, the buffer sizes, the sampler states), with the value each
// 32-bit word of the constant buffers holds most often.
//
//     rk_gen_shader_table <legacy install> <enhanced install> <out.inc> [--every=N] [--threads=N]
//
// --every=N reads every Nth drawable file of each install (default 1: all of them; an hour or so on eight threads).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include "ragekit/ragekit.h"

using namespace rk;

namespace {

std::string lower(std::string s) { for (auto& c : s) c = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); return s; }

/// Every drawable of a file (a .yft's main, array, cloth and physics children's drawables too).
std::vector<const Drawable*> drawables_of(const std::string& ext, const Bytes& file, DrawableDictionary& dd, Drawable& d, Fragment& f) {
    std::vector<const Drawable*> out;
    if (ext == ".ydr") { d = read_ydr(file); out.push_back(&d); }
    else if (ext == ".ydd") { dd = read_ydd(file); for (auto& x : dd.drawables) out.push_back(&x); }
    else if (ext == ".yft") {
        f = read_yft(file);
        if (f.drawable) out.push_back(&*f.drawable);
        if (f.drawable_cloth) out.push_back(&*f.drawable_cloth);
        for (auto& x : f.drawable_array) out.push_back(&x);
        for (auto& lod : f.physics)
            if (lod) for (auto& c : lod->children) { if (c.drawable) out.push_back(&*c.drawable); if (c.drawable_damaged) out.push_back(&*c.drawable_damaged); }
    }
    return out;
}

/// Runs `fn(file, bytes)` over every Nth .ydr/.ydd/.yft of an install on a few threads.
void scan(const GameIndex& gi, size_t every, int threads, const std::function<void(const std::string& ext, const Bytes&)>& fn) {
    std::vector<const IndexedFile*> files;
    std::map<std::string, size_t> seen;
    for (const IndexedFile& f : gi.files()) {
        const std::string& ext = gi.extension(f);
        if (ext != ".ydr" && ext != ".ydd" && ext != ".yft") continue;
        if (seen[ext]++ % every) continue;
        files.push_back(&f);
    }
    std::atomic<size_t> next{0}, done{0};
    std::mutex say;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++)
        pool.emplace_back([&] {
            for (;;) {
                const size_t k = next.fetch_add(1);
                if (k >= files.size()) return;
                const IndexedFile& f = *files[k];
                try { fn(gi.extension(f), gi.extract(f)); } catch (const std::exception&) {}
                const size_t n = ++done;
                if (n % 2000 == 0) { std::lock_guard<std::mutex> g(say); std::fprintf(stderr, "  %zu / %zu\n", n, files.size()); }
            }
        });
    for (auto& th : pool) th.join();
}

struct Layout {
    std::vector<ShaderParamGen9> params;
    std::vector<uint32_t> buffer_sizes;
    std::vector<uint8_t> samplers;
    uint8_t copies = 12;
    std::string key() const {
        std::string k;
        auto put = [&](const void* p, size_t n) { k.append((const char*)p, n); };
        for (auto& p : params) { put(&p.name_hash, 4); put(&p.data, 4); }
        k += '|';
        for (auto s : buffer_sizes) put(&s, 4);
        k += '|';
        k += std::to_string(samplers.size());   // the states number the file's samplers (see shader_gen9.cpp): not part of the layout
        k += (char)copies;
        return k;
    }
};

struct Seen {
    size_t count = 0;
    Layout layout;
    std::vector<std::map<uint32_t, size_t>> words;   // per 32-bit word of the buffers: value -> count
};

}   // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: rk_gen_shader_table <legacy install> <enhanced install> <out.inc> [--every=N] [--threads=N]\n");
        return 2;
    }
    size_t every = 1;
    int threads = (int)std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    for (int i = 4; i < argc; i++) {
        const std::string a = argv[i];
        if (a.rfind("--every=", 0) == 0) every = std::max<size_t>(1, std::stoul(a.substr(8)));
        else if (a.rfind("--threads=", 0) == 0) threads = std::max(1, std::stoi(a.substr(10)));
    }
    GameIndex legacy = GameIndex::open(argv[1]);
    GameIndex enhanced = GameIndex::open(argv[2]);
    if (legacy.info().game != Game::Legacy || enhanced.info().game != Game::Enhanced) {
        std::fprintf(stderr, "the first install must be Legacy, the second Enhanced\n");
        return 2;
    }

    // ---- Legacy effects: parameters, and each variable's Enhanced name
    std::fprintf(stderr, "Legacy effects...\n");
    ShaderDb db = ShaderDb::from_game(legacy);
    std::map<uint32_t, std::map<uint32_t, std::string>> gen9_names;   // shader -> legacy param hash -> Enhanced name
    std::map<uint32_t, std::map<uint32_t, std::string>> legacy_names; // shader -> legacy param hash -> Legacy name
    {
        std::map<std::string, const IndexedFile*> newest;
        for (const IndexedFile& f : legacy.files())
            if (legacy.extension(f) == ".fxc" && lower(legacy.path(f)).find("win32_40_final") != std::string::npos) newest[legacy.file_name(f)] = &f;   // ascending rank: the last is the newest
        for (auto& [name, f] : newest) {
            try {
                FxcFile fx = read_fxc(legacy.extract(*f));
                const uint32_t sh = joaat_lower(name.substr(0, name.size() - 4));
                for (auto& v : fx.variables) {
                    gen9_names[sh][joaat_lower(v.name)] = v.name2.empty() ? v.name : v.name2;
                    legacy_names[sh][joaat_lower(v.name)] = v.name;
                }
            } catch (const std::exception&) {}
        }
    }

    // ---- Legacy drawables: the preset and unk12 per shader and bucket
    std::fprintf(stderr, "Legacy drawables...\n");
    std::mutex mu;
    std::map<std::pair<uint32_t, uint8_t>, std::map<std::pair<uint32_t, uint16_t>, size_t>> presets;
    // the parameter lists the game's Legacy drawables carry per shader (names, types, counts, slots), with each value's count
    struct LegacySeen { size_t count = 0; std::vector<ShaderParam> params; std::vector<std::map<std::string, size_t>> values; };
    std::map<uint32_t, std::map<std::string, LegacySeen>> legacy_lists;
    std::map<std::pair<uint32_t, uint32_t>, std::map<uint64_t, size_t>> legacy_layouts;   // (shader, semantics) -> types -> count
    auto list_key = [](const Shader& s) {
        std::string k;
        for (auto& p : s.params) {
            k.append((const char*)&p.name_hash, 4);
            k += (char)p.type; k += (char)p.values.size(); k += (char)p.slot;
        }
        return k;
    };
    scan(legacy, every, threads, [&](const std::string& ext, const Bytes& file) {
        DrawableDictionary dd; Drawable d; Fragment f;
        const auto ds = drawables_of(ext, file, dd, d, f);
        std::lock_guard<std::mutex> g(mu);
        for (const Drawable* x : ds) {
            if (!x->shader_group) continue;
            for (const Model* m : x->all_models())
                for (auto& g : m->geometries)
                    if (g.shader_index < x->shader_group->shaders.size())
                        legacy_layouts[{x->shader_group->shaders[g.shader_index].name_hash, g.layout.flags}][g.layout.types]++;
            for (auto& s : x->shader_group->shaders) {
                presets[{s.name_hash, s.render_bucket}][{s.sps_hash, s.unk12}]++;
                LegacySeen& ls = legacy_lists[s.name_hash][list_key(s)];
                if (ls.count++ == 0) { ls.params = s.params; ls.values.resize(s.params.size()); }
                for (size_t i = 0; i < s.params.size(); i++)
                    if (s.params[i].type != ParamType::Texture)
                        ls.values[i][std::string((const char*)s.params[i].values.data(), s.params[i].values.size() * sizeof(Vec4))]++;
            }
        }
    });

    // ---- Enhanced drawables: each shader's layouts and the values of their buffers
    std::fprintf(stderr, "Enhanced drawables...\n");
    std::map<uint32_t, std::map<std::string, Seen>> layouts;
    std::map<uint32_t, std::map<uint8_t, size_t>> sampler_values;   // RK_TABLE_DIFFS: sampler name -> state -> count
    scan(enhanced, every, threads, [&](const std::string& ext, const Bytes& file) {
        DrawableDictionary dd; Drawable d; Fragment f;
        const auto ds = drawables_of(ext, file, dd, d, f);
        std::lock_guard<std::mutex> g(mu);
        for (const Drawable* x : ds) {
            if (!x->shader_group) continue;
            for (auto& s : x->shader_group->shaders) {
                if (!s.gen9) continue;
                Layout l{s.gen9->params, s.gen9->buffer_sizes, s.gen9->samplers, s.gen9->copies};
                if (std::getenv("RK_TABLE_DIFFS"))
                    for (auto& p : s.gen9->params)
                        if (p.kind() == ShaderParamGen9::Sampler && p.slot() < s.gen9->samplers.size()) sampler_values[p.name_hash][s.gen9->samplers[p.slot()]]++;
                Seen& seen = layouts[s.name_hash][l.key()];
                if (seen.count++ == 0) { seen.layout = l; seen.words.resize(s.gen9->buffers.size() / 4); }
                for (size_t w = 0; w < seen.words.size() && (w + 1) * 4 <= s.gen9->buffers.size(); w++) {
                    uint32_t v;
                    std::memcpy(&v, s.gen9->buffers.data() + w * 4, 4);
                    seen.words[w][v]++;
                }
            }
        }
    });

    if (std::getenv("RK_TABLE_DIFFS"))
        for (auto& [n, m] : sampler_values) {
            std::fprintf(stderr, "sampler %s:", hash_name(n).c_str());
            for (auto& [v, c] : m) std::fprintf(stderr, " %u x%zu", v, c);
            std::fprintf(stderr, "\n");
        }
    // ---- the table
    char buf[512];
    std::ofstream out(argv[3], std::ios::binary);
    std::string shaders, params, sizes, samplers, defaults, legacy_rows, floats, preset_rows;
    size_t nparams = 0, nsizes = 0, nsamplers = 0, ndefaults = 0, nlegacy = 0, nfloats = 0, npresets = 0, conflicts = 0;
    auto hex = [&](uint32_t v) { std::snprintf(buf, sizeof buf, "0x%08X", v); return std::string(buf); };
    auto flt = [&](float f) {
        if (f == 0) return std::string("0");
        uint32_t u; std::memcpy(&u, &f, 4);
        std::snprintf(buf, sizeof buf, "%.9g", f);
        std::string s = buf;
        float back = std::strtof(s.c_str(), nullptr);
        if (std::memcmp(&back, &f, 4) != 0 || !std::isfinite(f)) { std::snprintf(buf, sizeof buf, "bits(0x%08Xu)", u); return std::string(buf); }
        if (s.find_first_of(".e") == std::string::npos) s += ".0";
        return s + "f";
    };
    for (auto& [sh, by_key] : layouts) {
        const Seen* best = nullptr;
        for (auto& [k, s] : by_key) if (!best || s.count > best->count || (s.count == best->count && s.layout.params.size() > best->layout.params.size())) best = &s;
        if (by_key.size() > 1) {
            conflicts++;
            if (std::getenv("RK_TABLE_DIFFS")) {
                std::fprintf(stderr, "%s: %zu layouts\n", hash_name(sh).c_str(), by_key.size());
                for (auto& [k, s] : by_key) {
                    std::string d;
                    for (auto& p : s.layout.params) { std::snprintf(buf, sizeof buf, "%08X:%08X ", p.name_hash, p.data); d += buf; }
                    d += "| sizes"; for (auto z : s.layout.buffer_sizes) d += " " + std::to_string(z);
                    d += " | samplers"; for (auto z : s.layout.samplers) d += " " + std::to_string(z);
                    std::fprintf(stderr, "  %6zu  %s\n", s.count, d.c_str());
                }
            }
        }
        std::string sname;
        for (auto& n : db.base_names()) if (joaat_lower(n) == sh) sname = n;
        const auto& g9n = gen9_names[sh];
        const auto& lgn = legacy_names[sh];
        // Legacy parameters of the shader (its own preset)
        std::vector<ShaderParam> lp;
        if (auto it = legacy_lists.find(sh); it != legacy_lists.end()) {
            // the list the game's Legacy drawables carry most, with each parameter's most common value
            const LegacySeen* lbest = nullptr;
            for (auto& [k, ls] : it->second) if (!lbest || ls.count > lbest->count) lbest = &ls;
            lp = lbest->params;
            for (size_t i = 0; i < lp.size(); i++) {
                lp[i].texture.clear();
                if (lp[i].type == ParamType::Texture) continue;
                size_t mc = 0;
                for (auto& [v, n] : lbest->values[i]) if (n > mc) { mc = n; std::memcpy(lp[i].values.data(), v.data(), v.size()); }
            }
        } else if (!sname.empty()) {
            const ShaderPresetDef* own = db.preset(sname);
            ShaderPresetDef tmp{sname, sname, 0};
            lp = db.instantiate(own ? *own : tmp).params;
        }
        const size_t p0 = nparams;
        for (auto& p : best->layout.params) {
            uint32_t leg = 0;
            std::string lname;
            for (auto& q : lp) {
                auto it = g9n.find(q.name_hash);
                if (it != g9n.end() && joaat_lower(it->second) == p.name_hash) { leg = q.name_hash; lname = lgn.count(q.name_hash) ? lgn.at(q.name_hash) : ""; }
            }
            std::string g9 = hash_name(p.name_hash);
            for (auto& [lh, n] : g9n) if (joaat_lower(n) == p.name_hash) g9 = n;
            std::snprintf(buf, sizeof buf, "    {%s, %s, %s},   // %s%s%s\n", hex(p.name_hash).c_str(), hex(p.data).c_str(), hex(leg).c_str(), g9.c_str(),
                          leg ? " <- " : "", lname.c_str());
            params += buf;
            nparams++;
        }
        const size_t s0 = nsizes;
        for (auto s : best->layout.buffer_sizes) { sizes += std::to_string(s) + ", "; nsizes++; }
        const size_t m0 = nsamplers;
        for (auto s : best->layout.samplers) { samplers += std::to_string(s) + ", "; nsamplers++; }
        const size_t d0 = ndefaults;
        for (auto& w : best->words) {
            uint32_t mode = 0; size_t mc = 0;
            for (auto& [v, n] : w) if (n > mc) { mc = n; mode = v; }
            defaults += hex(mode) + ", ";
            if (++ndefaults % 8 == 0) defaults += "\n    ";
        }
        const size_t l0 = nlegacy;
        for (auto& q : lp) {
            const uint8_t count = q.type == ParamType::Texture ? 0 : (uint8_t)q.values.size();
            std::snprintf(buf, sizeof buf, "    {%s, %u, %u, %zu},   // %s\n", hex(q.name_hash).c_str(), count, (unsigned)q.slot, nfloats / 4,
                          lgn.count(q.name_hash) ? lgn.at(q.name_hash).c_str() : "");
            legacy_rows += buf;
            nlegacy++;
            if (count) {
                for (auto& v : q.values) { floats += flt(v.x) + ", " + flt(v.y) + ", " + flt(v.z) + ", " + flt(v.w) + ",\n    "; nfloats += 4; }
            }
        }
        const size_t r0 = npresets;
        for (int bucket = 0; bucket < 16; bucket++) {
            auto it = presets.find({sh, (uint8_t)bucket});
            uint32_t sps = 0; uint16_t unk12 = 32768; size_t best_n = 0;
            if (it != presets.end()) for (auto& [v, n] : it->second) if (n > best_n) { best_n = n; sps = v.first; unk12 = v.second; }
            if (!best_n && !sname.empty()) if (const ShaderPresetDef* pd = db.preset_for(sname, bucket); pd && pd->bucket == bucket) sps = joaat_lower(pd->name + ".sps");
            if (!sps) continue;
            std::snprintf(buf, sizeof buf, "    {%s, %d, %u},\n", hex(sps).c_str(), bucket, unk12);
            preset_rows += buf;
            npresets++;
        }
        std::snprintf(buf, sizeof buf, "    {%s, %zu, %zu, %zu, %zu, %zu, %zu, %zu, %zu, %zu, %zu, %zu},   // %s (%zu drawables%s)\n", hex(sh).c_str(), p0,
                      nparams - p0, s0, nsizes - s0, m0, nsamplers - m0, d0, l0, nlegacy - l0, r0, npresets - r0,
                      sname.empty() ? hash_name(sh).c_str() : sname.c_str(), best->count, by_key.size() > 1 ? ", other layouts seen" : "");
        shaders += buf;
    }
    // Legacy layouts that are not the standard table's: what the game's Legacy drawables use most for a shader and semantics
    std::string layout_rows;
    size_t nlayouts = 0;
    for (auto& [k, m] : legacy_layouts) {
        uint64_t best = 0; size_t bn = 0;
        for (auto& [t, n] : m) if (n > bn) { bn = n; best = t; }
        uint64_t mask = 0;
        for (int i = 0; i < 16; i++) if ((k.second >> i) & 1) mask |= 0xFull << (i * 4);
        if ((best & mask) == (VertexLayout::kTypesStandard & mask)) continue;
        std::snprintf(buf, sizeof buf, "    {%s, %s, 0x%016llXull},   // %zu geometries\n", hex(k.first).c_str(), hex(k.second).c_str(), (unsigned long long)best, bn);
        layout_rows += buf;
        nlayouts++;
    }
    if (nparams == 0) params = "    {0, 0, 0},\n";
    out << "// Generated by tools/gen_shader_table.cpp from GTA V Legacy " << legacy.info().exe_version << " and Enhanced: do not edit.\n"
        << "// Every shader of the game's Enhanced drawables: its parameter table (Enhanced name, kind/slot/offset/length, Legacy name),\n"
        << "// constant buffer sizes, sampler states and the buffers' most common contents; its Legacy parameters (name, values, slot)\n"
        << "// with the effect's defaults; the presets the game's Legacy drawables use per draw bucket (sps, bucket, unk12).\n"
        << "static constexpr float bits(uint32_t u) { return std::bit_cast<float>(u); }\n"
        << "static const ShaderRow kShaders[] = {\n" << shaders << "};\n"
        << "static const ParamRow kParams[] = {\n" << params << "};\n"
        << "static const uint32_t kBufferSizes[] = {" << (nsizes ? sizes : "0") << "};\n"
        << "static const uint8_t kSamplers[] = {" << (nsamplers ? samplers : "0") << "};\n"
        << "static const uint32_t kDefaults[] = {\n    " << (ndefaults ? defaults : "0") << "\n};\n"
        << "static const LegacyRow kLegacy[] = {\n" << (nlegacy ? legacy_rows : "    {0, 0, 0, 0},\n") << "};\n"
        << "static const float kFloats[] = {\n    " << (nfloats ? floats : "0") << "\n};\n"
        << "static const PresetRow kPresets[] = {\n" << (npresets ? preset_rows : "    {0, 0, 0},\n") << "};\n"
        << "static const LayoutRow kLayouts[] = {\n" << (nlayouts ? layout_rows : "    {0, 0, 0},\n") << "};\n";
    std::fprintf(stderr, "%zu shaders (%zu with more than one layout), %zu parameters, %zu Legacy parameters, %zu presets\n", layouts.size(), conflicts, nparams,
                 nlegacy, npresets);
    return 0;
}
