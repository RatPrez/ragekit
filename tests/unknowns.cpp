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
        };
        for (const std::string& t : topics) {
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
