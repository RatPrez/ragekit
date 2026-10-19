// rk_unknowns: statistics of the fields ragekit does not name yet (and of the ones it named from such statistics), over
// a game install: per field the values it takes, how often, and what they go with. docs/ quote its output as evidence.
//
//   rk_unknowns [--game=legacy|enhanced|<dir>] [--every=N] <topic>...
//
// Topics: ywr
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "ragekit/ragekit.h"

using namespace rk;

namespace {

/// Values and how often each came up; prints the most common ones.
struct Hist {
    std::map<double, size_t> values;
    size_t n = 0;
    double lo = INFINITY, hi = -INFINITY;
    void add(double v) {
        n++;
        if (values.size() < 100000) values[v]++;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    void print(const char* name, size_t top = 8, bool hex = false) const {
        std::printf("  %-28s n %zu, %zu distinct", name, n, values.size());
        if (n) std::printf(", %g .. %g", lo, hi);
        std::vector<std::pair<size_t, double>> v;
        for (auto& [x, c] : values) v.push_back({c, x});
        std::sort(v.rbegin(), v.rend());
        std::printf(":");
        for (size_t i = 0; i < v.size() && i < top; i++) {
            if (hex) std::printf(" 0x%llX", (unsigned long long)v[i].second);
            else std::printf(" %g", v[i].second);
            std::printf(" (%zu)", v[i].first);
        }
        std::printf("\n");
    }
};

struct Walk {
    const GameIndex& gi;
    size_t every;
    /// Calls f with every Nth file of an extension (bytes extracted); errors are counted and skipped.
    void each(const std::string& ext, const std::function<void(const IndexedFile&, const Bytes&)>& f) const {
        size_t seen = 0, errors = 0, done = 0;
        for (const IndexedFile& file : gi.files()) {
            if (gi.extension(file) != ext || seen++ % every) continue;
            try { f(file, gi.extract(file)); done++; } catch (const std::exception& e) {
                if (errors++ < 3) std::printf("  (%s: %s)\n", gi.path(file).c_str(), e.what());
            }
        }
        std::printf("  [%zu %s files, %zu errors]\n", done, ext.c_str(), errors);
    }
};

void topic_ywr(const Walk& w) {
    std::printf("ywr: the 8 bytes after each waypoint's position\n");
    Hist flags, heading, speed, u2, u3;
    w.each(".ywr", [&](const IndexedFile&, const Bytes& b) {
        for (const WaypointRecordEntry& e : read_ywr(b).entries) {
            flags.add(e.flags); heading.add(e.heading); speed.add(e.speed); u2.add(e.unk2); u3.add(e.unk3);
        }
    });
    flags.print("flags", 12, true); heading.print("heading", 12); speed.print("speed", 12); u2.print("unk2", 12, true); u3.print("unk3", 12, true);
    // the heading against the direction to the next point (as 0..255 of a turn, both conventions), the speed against
    // the distance to the next point
    Hist diff_ccw, diff_cw;
    std::map<int, std::pair<double, size_t>> step_by_hi;
    w.each(".ywr", [&](const IndexedFile&, const Bytes& b) {
        const WaypointRecording r = read_ywr(b);
        for (size_t i = 0; i + 1 < r.entries.size(); i++) {
            const Vec3 d = r.entries[i + 1].position - r.entries[i].position;
            const double len = std::sqrt(d.x * d.x + d.y * d.y);
            if (len < 0.2) continue;
            const double dir = std::atan2(-d.x, d.y) * 128 / 3.14159265358979;   // the game's heading: 0 = +y, counter-clockwise
            auto wrap = [](double x) { x = std::fmod(x, 256.0); if (x < -128) x += 256; if (x >= 128) x -= 256; return std::round(x); };
            diff_ccw.add(wrap(r.entries[i].heading - (dir < 0 ? dir + 256 : dir)));
            diff_cw.add(wrap(r.entries[i].heading + dir));
            auto& s = step_by_hi[r.entries[i].speed];
            s.first += std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            s.second++;
        }
    });
    diff_ccw.print("heading - direction (ccw)", 8); diff_cw.print("heading + direction (cw)", 8);
    std::printf("  mean distance to the next point by speed:");
    for (auto& [hi, s] : step_by_hi) if (s.second > 500) std::printf(" %d: %.2f m (%zu)", hi, s.first / s.second, s.second);
    std::printf("\n");
}

void topic_ynd(const Walk& w) {
    std::printf("ynd: the path nodes' unnamed fields\n");
    Hist unused, unk14, unk6, count2_same, unk48, lflags, street_known;
    w.each(".ynd", [&](const IndexedFile&, const Bytes& b) {
        const PathNodes n = read_ynd(b);
        for (const PathNode& p : n.nodes) {
            for (uint32_t u : p.unused) unused.add(u);
            unk14.add(p.unused14);
            lflags.add(p.link_count_flags & 7);
        }
        for (const PathJunctionRef& r : n.junction_refs) unk6.add(r.unused6);
        count2_same.add(n.junction_ref_count2 == n.junction_refs.size());
        unk48.add(n.unk48);
    });
    unused.print("node unused[4]", 6, true); unk14.print("node unk14", 12, true); lflags.print("link_count_flags & 7", 8);
    unk6.print("junction ref unk6", 8, true); count2_same.print("ref count2 == count", 4); unk48.print("root unk48", 4);
}

void topic_ycd(const Walk& w) {
    std::printf("ycd: the bone tracks' second byte against the track, the animations' flags and values\n");
    std::map<int, Hist> format_by_track;
    Hist flags, unk11, unk04, hash1c, usage, clip_unk30, usage_minus_refs, flags_root, flags_noroot;
    w.each(".ycd", [&](const IndexedFile&, const Bytes& b) {
        const ClipDictionary d = read_ycd(b);
        for (const AnimationRef& a : d.animations()) {
            for (const AnimationBoneId& id : a.bone_ids()) format_by_track[id.track].add(id.format);
            flags.add(a.flags()); unk11.add(a.block->get<uint8_t>(0x11)); unk04.add(a.block->get<uint32_t>(0x04));
            hash1c.add(a.block->get<uint32_t>(0x1C)); usage.add(a.block->get<uint32_t>(0x3C));
        }
        std::map<const RscBlock*, int> refs;
        for (const ClipRef& c : d.clips()) {
            clip_unk30.add(c.block->get<uint32_t>(0x30));
            for (const RscBlockPtr& a : c.animations()) refs[a.get()]++;
        }
        for (const AnimationRef& a : d.animations()) {
            usage_minus_refs.add((double)a.block->get<uint32_t>(0x3C) - refs[a.block.get()]);
            bool root_motion = false;
            for (const RscBlockPtr& s : a.sequences()) root_motion |= s->get<uint8_t>(0x1F) != 0;
            (root_motion ? flags_root : flags_noroot).add(a.flags());
        }
    });
    usage_minus_refs.print("usage - clips using it", 6);
    flags_root.print("flags, root motion", 6, true); flags_noroot.print("flags, no root motion", 6, true);
    for (auto& [t, h] : format_by_track) { char n[32]; std::snprintf(n, sizeof n, "format on track %d", t); h.print(n, 6); }
    flags.print("animation flags (0x10)", 8, true); unk11.print("animation 0x11", 4); unk04.print("animation 0x04", 4);
    hash1c.print("animation 0x1C", 8, true); usage.print("animation usage (0x3C)", 8); clip_unk30.print("clip 0x30", 4);
}

void textures_of(const TextureDictionary& t, Hist& unk32, Hist& layers, Hist& conv, Hist& unk23, Hist& unk23_mips, Hist& unk23_nomips) {
    for (const Texture& x : t.textures) {
        unk32.add(x.unk32); layers.add(x.layer_count()); conv.add(x.conversion_flags());
        if (x.gen9) { unk23.add(x.gen9->unk23); (x.levels > 1 ? unk23_mips : unk23_nomips).add(x.gen9->unk23); }
    }
}

void topic_texture(const Walk& w) {
    std::printf("texture: byte 0x32 / 0x33 and Enhanced's byte 0x23\n");
    Hist unk32, layers, conv, unk23, unk23_mips, unk23_nomips;
    std::map<int, Hist> unk23_by_format;
    std::map<std::string, Hist> by_key;
    w.each(".ytd", [&](const IndexedFile& file, const Bytes& b) {
        const TextureDictionary t = read_ytd(b);
        textures_of(t, unk32, layers, conv, unk23, unk23_mips, unk23_nomips);
        int zero = 0, set = 0;
        for (const Texture& x : t.textures) if (x.gen9) (x.gen9->unk23 ? set : zero)++;
        if (zero || set) by_key[zero && set ? "files: unk23 mixed" : zero ? "files: unk23 all 0" : "files: unk23 all 0x28"].add(t.textures.size());
        const std::string p = w.gi.path(file);
        by_key[std::string("unk23 by archive: ") + (p.find("dlcpacks") != std::string::npos ? "dlc" : p.rfind("update", 0) == 0 ? "update" : "base")].add(set ? (zero ? 2 : 1) : 0);
        for (const Texture& x : t.textures) {
            if (!x.gen9) continue;
            unk23_by_format[(int)x.format].add(x.gen9->unk23);
            uint32_t full = 1;
            for (uint32_t s = std::max(x.width, x.height); s > 1; s >>= 1) full++;
            char k[64];
            std::snprintf(k, sizeof k, "unk23, %s chain", x.levels == full ? "full" : x.levels == 1 ? "no" : "partial");
            by_key[k].add(x.gen9->unk23);
            std::snprintf(k, sizeof k, "unk23, usage %u", (unsigned)x.usage());
            by_key[k].add(x.gen9->unk23);
            std::snprintf(k, sizeof k, "unk23, field 19-23 = %u", (x.usage_data >> 19) & 0x1F);
            by_key[k].add(x.gen9->unk23);
            std::snprintf(k, sizeof k, "unk23, flags 0x%X", x.gen9->flags);
            by_key[k].add(x.gen9->unk23);
            std::snprintf(k, sizeof k, "unk23, extra 0x%X", x.extra_flags);
            by_key[k].add(x.gen9->unk23);
        }
    });
    for (auto& [k, h] : by_key) if (h.n > 20) h.print(k.c_str(), 3, true);
    unk32.print("unk32 (0x32-0x33)", 10, true); layers.print("layer count (0x33)", 6); conv.print("conversion flags", 8, true);
    unk23.print("gen9 unk23", 6, true); unk23_mips.print("gen9 unk23, with mips", 6, true); unk23_nomips.print("gen9 unk23, one level", 6, true);
    for (auto& [f, h] : unk23_by_format) { char n[48]; std::snprintf(n, sizeof n, "gen9 unk23, %s", texture_format_name((TextureFormat)f)); h.print(n, 4, true); }
}

void topic_drawable(const Walk& w) {
    std::printf("drawable: shader 0x12, skeleton 0x1C, light 0x45 / 0x46, drawable 0x3C / 0x4C\n");
    Hist unk12, sk1c, sk1c_vs_bones, l45, l46, d3c, d4c;
    w.each(".ydr", [&](const IndexedFile&, const Bytes& b) {
        const Drawable d = read_ydr(b);
        if (d.shader_group) for (const Shader& s : d.shader_group->shaders) unk12.add(s.unk12);
        if (d.skeleton) {
            sk1c.add(d.skeleton->unk1C);
            sk1c_vs_bones.add(d.skeleton->bones.size() == 1 ? (d.skeleton->unk1C ? 1 : 0) : d.skeleton->unk1C ? 3 : 2);
        }
        for (const Light& l : d.lights) { l45.add(l.unk45); l46.add(l.unk46); }
        d3c.add(d.unk3C); d4c.add(d.unk4C);
    });
    unk12.print("shader unk12", 8, true); sk1c.print("skeleton unk1C", 8); sk1c_vs_bones.print("skeleton: 0/1 one bone, unk1C 0/!0; 2/3 more", 8);
    l45.print("light unk45", 8, true); l46.print("light unk46", 8, true); d3c.print("drawable unk3C", 4, true); d4c.print("drawable unk4C", 4, true);
}

void bound_stats(const Bound& b, std::map<std::string, Hist>& h) {
    char k[64];
    std::snprintf(k, sizeof k, "unk3C %s", bound_type_name(b.type)); h[k].add(b.unk3C);
    h["unk_flags"].add(b.unk_flags); h["unk5E"].add(b.unk5E);
    if (b.type == BoundType::Capsule) h["capsule unk70 - half height"].add(b.unk70 - (b.box_max.z - b.box_min.z) / 2 + b.sphere_radius);
    else { std::snprintf(k, sizeof k, "unk70 %s", bound_type_name(b.type)); h[k].add(b.unk70); }
    if (b.geometry) {
        h["geometry unk80"].add(b.geometry->unk80); h["geometry unk82"].add(b.geometry->unk82);
        h["geometry unk82, vertex colours"].add(b.geometry->unk82 - (b.geometry->vertex_colours.empty() ? 0 : 1));
        h["geometry unk9C"].add(b.geometry->unk9C); h["geometry unkAC"].add(b.geometry->unkAC);
        h["geometry unkAC - margin"].add(b.geometry->unkAC - b.margin);
        h["geometry unk9C x 32767 - margin"].add(b.geometry->unk9C * 32767 - b.margin);
        if (b.type == BoundType::GeometryBVH) h["bvh unk140"].add(b.geometry->unk140);
    }
    for (const BoundChild& c : b.children) if (c.bound) bound_stats(*c.bound, h);
}

void topic_bounds(const Walk& w) {
    std::printf("bounds: phBound and phBoundGeometry fields\n");
    std::map<std::string, Hist> h;
    w.each(".ybn", [&](const IndexedFile&, const Bytes& b) { if (auto x = read_ybn(b)) bound_stats(*x, h); });
    for (auto& [k, v] : h) v.print(k.c_str(), 6, k.find("unk9C") == std::string::npos && k.find("unkAC") == std::string::npos);
}

void topic_fragment(const Walk& w) {
    std::printf("fragment: fragType 0xB0 .. 0xCC, the bone transforms' u16, the physics LOD's unknown data and per child values\n");
    Hist b0, b8, bc, c0, c4, cc, btu, btu_vs, d1, d2, uf, uv;
    Hist b0_minus_children, b8_mod16;
    w.each(".yft", [&](const IndexedFile&, const Bytes& b) {
        const Fragment f = read_yft(b);
        b0.add(f.unkB0); b8.add(f.unkB8); bc.add(f.unkBC); c0.add(f.unkC0); c4.add(f.unkC4); cc.add(f.unkCC);
        btu.add(f.bone_transforms_unk); btu_vs.add((double)f.bone_transforms_unk - f.bone_transforms.size());
        b8_mod16.add(f.unkB8 % 16);
        for (const auto& l : f.physics) {
            if (!l) continue;
            d1.add((double)l->unknown_data1.size()); d2.add((double)l->unknown_data2.size());
            for (const FragChild& ch : l->children) { uf.add(ch.unk_float); uv.add(ch.unk_vec.w); }
        }
        if (f.physics[0]) b0_minus_children.add((double)f.unkB0 - 16.0 * f.physics[0]->children.size());
    });
    b0.print("unkB0", 8); b8.print("unkB8", 8); b8_mod16.print("unkB8 % 16", 4); bc.print("unkBC", 4); c0.print("unkC0", 8, true); c4.print("unkC4", 8, true);
    cc.print("unkCC", 8); btu.print("bone transforms u16", 6); btu_vs.print("bone transforms u16 - count", 6);
    d1.print("lod unknown_data1 size", 6); d2.print("lod unknown_data2 size", 6); uf.print("child unk_float", 8); uv.print("child unk_vec.w", 6);
    b0_minus_children.print("unkB0 - 16 x children", 6);
}

// Which structures of the meta files the typed map model (map.h) reads into fields and which only come back through
// the generic layer (MetaObject / raw blocks), by how many items of each the install's files hold.
void topic_meta(const Walk& w) {
    std::printf("meta: structure types by items, typed (map.h fields) or generic only\n");
    static const char* typed[] = {"CMapTypes", "CMapData", "CBaseArchetypeDef", "CTimeArchetypeDef", "CMloArchetypeDef", "CMloRoomDef",
                                  "CMloPortalDef", "CMloEntitySet", "CMloTimeCycleModifier", "CEntityDef", "CMloInstanceDef", "CCarGen",
                                  "BoxOccluder", "CTimeCycleModifier", "CBlockDesc"};
    std::map<uint32_t, bool> is_typed;
    for (const char* t : typed) is_typed[joaat(t)] = true;
    struct Count { size_t files = 0, items = 0, bytes = 0; std::string ext; };
    std::map<uint32_t, Count> counts;
    std::map<uint32_t, std::string> names;
    for (const char* ext : {".ytyp", ".ymap", ".ymt"}) {
        w.each(ext, [&](const IndexedFile&, const Bytes& b) {
            if (!rsc::Resource::is_resource(b)) return;
            const Meta m = read_meta(b);
            std::map<uint32_t, bool> seen;
            for (const MetaBlock& blk : m.blocks) {
                const MetaStruct* s = m.find_struct(blk.type);
                if (!s || s->size <= 0) continue;   // strings, hashes, arrays of plain values
                Count& c = counts[blk.type];
                c.items += blk.data.size() / (size_t)s->size;
                c.bytes += blk.data.size();
                if (!seen[blk.type]) { c.files++; seen[blk.type] = true; }
                if (c.ext.find(ext) == std::string::npos) c.ext += ext;
            }
        });
    }
    std::vector<std::pair<size_t, uint32_t>> order;
    for (auto& [t, c] : counts) order.push_back({c.items, t});
    std::sort(order.rbegin(), order.rend());
    for (auto& [n, t] : order) {
        const Count& c = counts[t];
        std::printf("  %-44s %-7s %9zu items %6zu files %10zu bytes  %s\n", pso_name(t).c_str(), is_typed.count(t) ? "typed" : "generic", c.items, c.files, c.bytes,
                    c.ext.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string game;
    size_t every = 1;
    std::vector<std::string> topics;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a.rfind("--game=", 0) == 0) game = a.substr(7);
        else if (a.rfind("--every=", 0) == 0) every = std::max<size_t>(1, std::stoul(a.substr(8)));
        else topics.push_back(a);
    }
    if (topics.empty()) { std::printf("usage: rk_unknowns [--game=legacy|enhanced|<dir>] [--every=N] <topic>...\n"); return 2; }
    if (game.empty() || game == "legacy" || game == "enhanced") game = GameIndex::default_game_dir(game == "enhanced" ? Game::Enhanced : Game::Legacy);
    try {
        const GameIndex gi = GameIndex::open(game);
        const Walk w{gi, every};
        const std::map<std::string, void (*)(const Walk&)> table = {
            {"ywr", topic_ywr},
            {"ynd", topic_ynd},
            {"ycd", topic_ycd},
            {"texture", topic_texture},
            {"drawable", topic_drawable},
            {"bounds", topic_bounds},
            {"fragment", topic_fragment},
            {"meta", topic_meta},
        };
        for (const std::string& t : topics) {
            if (t.rfind("list.", 0) == 0) {   // list.<ext>: the paths of every file of a kind
                for (const IndexedFile& f : gi.files()) if (gi.extension(f) == t.substr(4)) std::printf("%s\n", gi.path(f).c_str());
                continue;
            }
            auto it = table.find(t);
            if (it == table.end()) { std::printf("unknown topic %s\n", t.c_str()); return 2; }
            it->second(w);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
