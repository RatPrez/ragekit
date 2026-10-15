#include "ragekit/audio_occlusion.h"
#include "ragekit/pso.h"
#include "ragekit/hash.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>

namespace rk {

uint32_t audio_hash(const std::string& s) {
    if (s.empty()) return 0;
    auto hex = [&](size_t from) { return (uint32_t)std::strtoul(s.c_str() + from, nullptr, 16); };
    if (s.size() == 13 && s.compare(0, 5, "hash_") == 0) return hex(5);
    if (s.size() == 10 && s.compare(0, 2, "0x") == 0) return hex(2);
    return joaat_lower(s);
}

uint32_t occlusion_proxy_hash(uint32_t h, const Vec3& p) {
    auto t = [](float v) { return (uint32_t)(int32_t)std::trunc(v * 100.0f); };
    return h ^ t(p.x) ^ t(p.y) ^ t(p.z);
}
uint32_t occlusion_proxy_hash(const std::string& archetype, const Vec3& p) { return occlusion_proxy_hash(joaat_lower(archetype), p); }
uint32_t AudioOcclusion::proxy_hash() const { return occlusion_proxy_hash(name.empty() ? name_hash : joaat_lower(name), position); }

// ---------------------------------------------------------------------------------------------------------------
// paths (a port of tools/audio_occlusion.py, itself ht_mlotool / gtav-audio-occlusion)

namespace {

struct Portal {
    int frm = 0, to = 0;
    bool mirror = false;
    int global_idx[2] = {-1, -1};
};
struct Node {
    int index = 0;
    uint32_t key = 0;
    std::vector<int> portals;                   // indices into the portal list
    std::vector<int> edges;                     // neighbour node indices
};
struct PathN {
    int origin = 0, dest = 0, distance = 0;
    uint64_t ukey = 0;
    std::vector<std::pair<uint64_t, int>> children; // (child ukey, global portal index)
};

uint64_t path_key(const std::vector<Node>& nodes, int origin, int dest, int distance) {
    if (origin == dest) return 0;
    return (uint64_t)(uint32_t)(nodes[(size_t)origin].key - nodes[(size_t)dest].key) + (uint64_t)distance;
}

} // namespace

OcclusionData build_occlusion(const AudioOcclusion& m) {
    const size_t nrooms = m.rooms.size();
    const uint32_t proxy = m.proxy_hash();
    std::vector<Portal> portals;
    for (auto& p : m.portals) {
        if (p.room_from >= nrooms || p.room_to >= nrooms) throw Error("occlusion: portal refers to a room that does not exist");
        Portal q; q.frm = (int)p.room_from; q.to = (int)p.room_to; q.mirror = p.mirror();
        portals.push_back(q);
    }
    // global portal index: per room, in portal order, each non-mirror portal touching the room gets one entry
    int g = 0;
    for (size_t ri = 0; ri < nrooms; ri++)
        for (auto& p : portals) {
            if (p.mirror) continue;
            if (p.frm == (int)ri) p.global_idx[0] = g++;
            else if (p.to == (int)ri) p.global_idx[1] = g++;
        }

    OcclusionData out;
    // PortalInfoList
    for (size_t ri = 0; ri < nrooms; ri++) {
        int local = 0;
        for (size_t pi = 0; pi < portals.size(); pi++) {
            const Portal& p = portals[pi];
            if (p.frm != (int)ri && p.to != (int)ri) continue;
            if (!p.mirror) {
                OcclusionPortalInfo e;
                e.interior_proxy_hash = e.dest_interior_hash = proxy;
                e.portal_idx = local; e.room_idx = (int32_t)ri;
                e.dest_room_idx = p.frm == (int)ri ? p.to : p.frm;
                for (auto& en : m.portals[pi].entities) {
                    OcclusionPortalEntity x;
                    x.link_type = en.link_type; x.max_occlusion = en.max_occlusion; x.is_door = en.is_door; x.is_glass = en.is_glass;
                    x.model_hash = en.model.empty() ? en.model_hash : audio_hash(en.model);
                    e.entities.push_back(x);
                }
                out.portals.push_back(std::move(e));
            }
            local++;
        }
    }

    // nodes
    std::vector<Node> nodes(nrooms);
    for (size_t ri = 0; ri < nrooms; ri++) {
        Node& n = nodes[ri];
        n.index = (int)ri;
        n.key = m.rooms[ri] == "limbo" ? joaat_lower("outside") : proxy ^ joaat_lower(m.rooms[ri]);
        for (size_t pi = 0; pi < portals.size(); pi++) {
            const Portal& p = portals[pi];
            if (!p.mirror && (p.frm == (int)ri || p.to == (int)ri)) n.portals.push_back((int)pi);
        }
    }
    for (auto& n : nodes) {
        std::vector<bool> seen(nrooms, false);
        for (int pi : n.portals) {
            const Portal& p = portals[(size_t)pi];
            int other = p.frm == n.index ? p.to : p.to == n.index ? p.frm : -1;
            if (other >= 0 && !seen[(size_t)other]) { seen[(size_t)other] = true; n.edges.push_back(other); }
        }
    }

    auto gidx = [&](const Portal& p, int edge_index) {
        if (p.to == edge_index) return p.global_idx[0];
        if (p.frm == edge_index) return p.global_idx[1];
        return -1;
    };
    std::unordered_map<uint64_t, std::unique_ptr<PathN>> paths;
    std::vector<std::vector<uint64_t>> keys((size_t)std::max(m.max_distance, 1) + 1);
    auto find = [&](int a, int b, int d) -> PathN* {
        auto it = paths.find((uint64_t)(uint32_t)(nodes[(size_t)a].key - nodes[(size_t)b].key) + (uint64_t)d);
        return it == paths.end() ? nullptr : it->second.get();
    };
    auto already = [&](int a, int b, int d) {
        for (int i = 1; i < d; i++) if (find(a, b, i)) return true;
        return false;
    };
    auto make = [&](int a, int b, int d) {
        auto p = std::make_unique<PathN>();
        p->origin = a; p->dest = b; p->distance = d; p->ukey = path_key(nodes, a, b, d);
        return p;
    };
    auto commit = [&](std::unique_ptr<PathN> p, int d) {
        uint64_t k = p->ukey;
        paths[k] = std::move(p);
        keys[(size_t)d].push_back(k);
    };

    for (int d = 1; d <= m.max_distance; d++) {
        if (d == 1) {
            for (auto& n : nodes)
                for (int e : n.edges) {
                    auto p = make(n.index, e, 1);
                    for (int pt : n.portals) {
                        int gi = gidx(portals[(size_t)pt], e);
                        if (gi >= 0) p->children.push_back({0, gi});
                    }
                    if (!p->children.empty()) commit(std::move(p), 1);
                }
        } else {
            for (auto& A : nodes) for (auto& B : nodes) {
                if (A.index == B.index) continue;
                const int a = A.index, b = B.index;
                const bool limbo_pair = a == 0 || b == 0;
                std::vector<int> edges;
                for (int e : A.edges) if (limbo_pair || e != 0) edges.push_back(e);
                for (uint64_t k : std::vector<uint64_t>(keys[(size_t)d - 1])) {
                    const PathN& path = *paths.at(k);
                    // either extend an existing path (new or existing node `cur` for a->b at distance d)
                    auto add_children = [&](auto&& pick) {
                        PathN* cur = find(a, b, d);
                        std::unique_ptr<PathN> fresh;
                        bool is_new = cur == nullptr;
                        if (is_new) { fresh = make(a, b, d); cur = fresh.get(); }
                        pick(*cur);
                        if (is_new && !cur->children.empty()) commit(std::move(fresh), d);
                    };
                    if (d <= 3) {
                        if (path.origin == a && path.dest == b) {
                            add_children([&](PathN& cur) {
                                for (int pt : A.portals) {
                                    int gi = gidx(portals[(size_t)pt], b);
                                    if (gi >= 0) cur.children.push_back({path_key(nodes, a, a, d - 1), gi});
                                }
                            });
                        } else {
                            for (int e : edges)
                                if (path.origin == e && path.dest == b)
                                    add_children([&](PathN& cur) {
                                        for (int pt : A.portals) {
                                            const Portal& P = portals[(size_t)pt];
                                            int gi = gidx(P, e);
                                            if (gi >= 0 && (e == P.frm || (P.frm != 0 && P.to != 0)))
                                                cur.children.push_back({path_key(nodes, e, b, d - 1), gi});
                                        }
                                    });
                        }
                    } else {
                        for (int e : edges)
                            if (path.origin == e && path.dest == b && !already(a, b, d))
                                add_children([&](PathN& cur) {
                                    for (int pt : A.portals) {
                                        const Portal& P = portals[(size_t)pt];
                                        int gi = gidx(P, e);
                                        if (gi >= 0 && (e == b || (P.frm != 0 && P.to != 0)))
                                            cur.children.push_back({path_key(nodes, e, b, d - 1), gi});
                                    }
                                });
                    }
                }
            }
        }
        std::sort(keys[(size_t)d].begin(), keys[(size_t)d].end());
    }

    // The game's order: room pairs sorted by (shortest path distance, unsigned pair key), each pair's nodes by distance.
    std::map<uint32_t, std::vector<std::pair<int, uint64_t>>> groups;
    for (int d = 1; d <= m.max_distance; d++)
        for (uint64_t k : keys[(size_t)d]) groups[(uint32_t)(k - (uint64_t)d)].push_back({d, k});
    std::vector<std::pair<std::pair<int, uint32_t>, std::vector<std::pair<int, uint64_t>>>> ordered;
    for (auto& [base, lst] : groups) {
        int mind = lst.front().first;
        for (auto& x : lst) mind = std::min(mind, x.first);
        ordered.push_back({{mind, base}, lst});
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](auto& x, auto& y) { return x.first < y.first; });
    for (auto& grp : ordered) {
        std::sort(grp.second.begin(), grp.second.end());
        for (auto& [d, k] : grp.second) {
            const PathN& p = *paths.at(k);
            if (p.children.empty()) continue;
            OcclusionPathNode node;
            node.key = (uint32_t)p.ukey;
            std::vector<std::pair<uint64_t, int>> ch = p.children;
            std::stable_sort(ch.begin(), ch.end(), [](auto& x, auto& y) { return x.second < y.second; });
            for (auto& c : ch) node.children.push_back({(uint32_t)c.first, c.second});
            out.nodes.push_back(std::move(node));
        }
    }
    return out;
}

AudioOcclusion occlusion_from_mlo(const Archetype& mlo, const Vec3& position) {
    if (!mlo.is_mlo()) throw Error("occlusion: archetype is not an MLO");
    AudioOcclusion m;
    m.position = position;
    m.name_hash = mlo.name; // the ytyp stores the hash only; callers set `name` when they know the string
    for (auto& r : mlo.rooms) m.rooms.push_back(r.name);
    for (auto& p : mlo.portals) {
        OcclusionPortal q;
        q.room_from = p.room_from; q.room_to = p.room_to; q.flags = p.flags;
        for (uint32_t idx : p.attached_objects) {
            if (idx >= mlo.entities.size()) continue;
            OcclusionEntity e;
            e.model_hash = mlo.entities[idx].archetype_name;
            q.entities.push_back(e);
        }
        m.portals.push_back(std::move(q));
    }
    return m;
}

// ---------------------------------------------------------------------------------------------------------------
// the ymt (PSO)

namespace {

struct Names {
    uint32_t entity = joaat("naOcclusionPortalEntityMetadata"), portal = joaat("naOcclusionPortalInfoMetadata"),
             child = joaat("naOcclusionPathNodeChildMetadata"), node = joaat("naOcclusionPathNodeMetadata"),
             root = joaat("naOcclusionInteriorMetadata");
};
const Names& N() { static const Names n; return n; }

int64_t geti(const PsoView& v, const PsoObject& o, const char* name) {
    PsoField f; int64_t x = 0;
    if (v.find(o, joaat(name), f)) v.get_int(f, x);
    return x;
}
template <class F> void each(const PsoView& v, const PsoObject& o, const char* name, F fn) {
    PsoField f;
    if (!v.find(o, joaat(name), f)) return;
    PsoArrayRef a = v.get_array(f);
    if (!a.valid) throw Error(std::string("occlusion: unreadable array ") + name);
    for (uint32_t i = 0; i < a.count; i++) {
        PsoObject e = v.array_struct(a, i);
        if (!e) throw Error(std::string("occlusion: array element without structure info in ") + name);
        fn(e);
    }
}

struct Block { uint32_t name; Bytes data; };
struct Ptr { uint32_t block = 0, offset = 0, count = 0; };
struct Builder {
    std::vector<Block> blocks;
    Ptr add(uint32_t name, const Bytes& d, uint32_t count) {
        size_t b = 0;
        for (; b < blocks.size(); b++) if (blocks[b].name == name) break;
        if (b == blocks.size()) blocks.push_back({name, {}});
        Ptr p{uint32_t(b + 1), uint32_t(blocks[b].data.size()), count};
        blocks[b].data.insert(blocks[b].data.end(), d.begin(), d.end());
        return p;
    }
    static void put32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
    static void put16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
    static void header(Bytes& b, const Ptr& p) {
        put32(b, p.block ? (p.block & 0xFFF) | ((p.offset & 0xFFFFF) << 12) : 0);
        put32(b, 0);
        put16(b, (uint16_t)p.count); put16(b, (uint16_t)p.count);
        put32(b, 0);
    }
};
PsoStructEntry entry(uint32_t name, uint8_t type, uint16_t off, uint32_t ref) {
    PsoStructEntry e; e.name = name; e.type = type; e.subtype = 0; e.offset = off; e.ref_key = ref;
    return e;
}
PsoElement structure(uint32_t name, int32_t size, std::vector<PsoStructEntry> entries) {
    PsoElement e; e.name = name; e.st.size = size; e.st.entries = std::move(entries);
    return e;
}

} // namespace

OcclusionData read_occlusion_ymt(std::span<const uint8_t> file) {
    if (!is_pso(file)) throw Error("occlusion: not a PSO file");
    Pso p = read_pso(file);
    if (!p.has_map || !p.has_schema) throw Error("occlusion: PSO without a readable PMAP / PSCH");
    PsoView v(p);
    PsoObject root = v.root();
    if (!root || root.type != N().root) throw Error("occlusion: root structure is not naOcclusionInteriorMetadata");
    OcclusionData d;
    each(v, root, "PortalInfoList", [&](const PsoObject& o) {
        OcclusionPortalInfo e;
        e.interior_proxy_hash = (uint32_t)geti(v, o, "InteriorProxyHash");
        e.portal_idx = (int32_t)geti(v, o, "PortalIdx");
        e.room_idx = (int32_t)geti(v, o, "RoomIdx");
        e.dest_interior_hash = (uint32_t)geti(v, o, "DestInteriorHash");
        e.dest_room_idx = (int32_t)geti(v, o, "DestRoomIdx");
        each(v, o, "PortalEntityList", [&](const PsoObject& x) {
            OcclusionPortalEntity en;
            en.link_type = (uint32_t)geti(v, x, "LinkType");
            PsoField f; double fl = 0;
            if (v.find(x, joaat("MaxOcclusion"), f) && v.get_float(f, fl)) en.max_occlusion = (float)fl;
            en.model_hash = (uint32_t)geti(v, x, "EntityModelHashkey");
            en.is_door = geti(v, x, "IsDoor") != 0;
            en.is_glass = geti(v, x, "IsGlass") != 0;
            e.entities.push_back(en);
        });
        d.portals.push_back(std::move(e));
    });
    each(v, root, "PathNodeList", [&](const PsoObject& o) {
        OcclusionPathNode n;
        n.key = (uint32_t)geti(v, o, "Key");
        each(v, o, "PathNodeChildList", [&](const PsoObject& c) {
            n.children.push_back({(uint32_t)geti(v, c, "PathNodeKey"), (int32_t)geti(v, c, "PortalInfoIdx")});
        });
        d.nodes.push_back(std::move(n));
    });
    return d;
}

Bytes write_occlusion_ymt(const OcclusionData& d) {
    using namespace psotype;
    const Names& n = N();
    Builder b;
    auto put32 = Builder::put32;

    // portal infos: each one's entities first (one shared block), then the array of infos
    Ptr portals;
    if (!d.portals.empty()) {
        Bytes all;
        for (auto& pi : d.portals) {
            Ptr pe;
            if (!pi.entities.empty()) {
                Bytes eb;
                for (auto& e : pi.entities) {
                    put32(eb, e.link_type);
                    uint32_t f; std::memcpy(&f, &e.max_occlusion, 4); put32(eb, f);
                    put32(eb, e.model_hash);
                    eb.push_back(e.is_door ? 1 : 0); eb.push_back(e.is_glass ? 1 : 0); eb.push_back(0); eb.push_back(0);
                }
                pe = b.add(n.entity, eb, (uint32_t)pi.entities.size());
            }
            put32(all, pi.interior_proxy_hash); put32(all, (uint32_t)pi.portal_idx); put32(all, (uint32_t)pi.room_idx);
            put32(all, pi.dest_interior_hash); put32(all, (uint32_t)pi.dest_room_idx); put32(all, 0);
            Builder::header(all, pe);
        }
        portals = b.add(n.portal, all, (uint32_t)d.portals.size());
    }
    Ptr nodes;
    if (!d.nodes.empty()) {
        Bytes all;
        for (auto& nd : d.nodes) {
            Ptr pc;
            if (!nd.children.empty()) {
                Bytes cb;
                for (auto& c : nd.children) { put32(cb, c.path_node_key); put32(cb, (uint32_t)c.portal_info_idx); }
                pc = b.add(n.child, cb, (uint32_t)nd.children.size());
            }
            put32(all, nd.key); put32(all, 0);
            Builder::header(all, pc);
        }
        nodes = b.add(n.node, all, (uint32_t)d.nodes.size());
    }
    Bytes rootb;
    Builder::header(rootb, portals);
    Builder::header(rootb, nodes);
    b.add(n.root, rootb, 1);
    uint32_t root_id = (uint32_t)b.blocks.size();

    Pso p;
    uint32_t ai = kArrayInfo;
    std::map<uint32_t, PsoElement> el;
    el[n.entity] = (structure(n.entity, 16, {entry(joaat("LinkType"), UInt, 0, 0), entry(joaat("MaxOcclusion"), Float, 4, 0),
        entry(joaat("EntityModelHashkey"), UInt, 8, 0), entry(joaat("IsDoor"), Bool, 12, 0), entry(joaat("IsGlass"), Bool, 13, 0)}));
    el[n.portal] = (structure(n.portal, 40, {entry(joaat("InteriorProxyHash"), UInt, 0, 0), entry(joaat("PortalIdx"), SInt, 4, 0),
        entry(joaat("RoomIdx"), SInt, 8, 0), entry(joaat("DestInteriorHash"), UInt, 12, 0), entry(joaat("DestRoomIdx"), SInt, 16, 0),
        entry(ai, Structure, 0, n.entity), entry(joaat("PortalEntityList"), Array, 24, 5)}));
    el[n.child] = (structure(n.child, 8, {entry(joaat("PathNodeKey"), UInt, 0, 0), entry(joaat("PortalInfoIdx"), SInt, 4, 0)}));
    el[n.node] = (structure(n.node, 24, {entry(joaat("Key"), UInt, 0, 0), entry(ai, Structure, 0, n.child),
        entry(joaat("PathNodeChildList"), Array, 8, 1)}));
    el[n.root] = (structure(n.root, 32, {entry(ai, Structure, 0, n.portal), entry(joaat("PortalInfoList"), Array, 0, 0),
        entry(ai, Structure, 0, n.node), entry(joaat("PathNodeList"), Array, 16, 2)}));

    // CodeWalker's XmlPso registers a structure info when the first item of that type has been built (children before
    // parents, in document order), so the schema order depends on the data: an Entity struct sits ahead of PortalInfo only
    // when the first portal info has entities, and no Entity struct exists when no portal has one.
    std::vector<uint32_t> reg;
    auto add_type = [&](uint32_t t) { if (std::find(reg.begin(), reg.end(), t) == reg.end()) reg.push_back(t); };
    for (auto& pi : d.portals) { if (!pi.entities.empty()) add_type(n.entity); add_type(n.portal); }
    for (auto& nd : d.nodes) { if (!nd.children.empty()) add_type(n.child); add_type(n.node); }
    add_type(n.root);
    for (uint32_t t : reg) p.schema.elements.push_back(el.at(t));

    PsoSection psin; psin.ident = kPsoPSIN;
    psin.payload.assign(8, 0);
    int32_t offset = 16;
    for (auto& blk : b.blocks) {
        PsoBlock pb; pb.name = blk.name; pb.offset = offset; pb.length = (int32_t)blk.data.size();
        p.map.blocks.push_back(pb);
        psin.payload.insert(psin.payload.end(), blk.data.begin(), blk.data.end());
        offset += (int32_t)blk.data.size();
    }
    p.map.root_id = (int32_t)root_id;
    p.has_map = p.has_schema = true;
    PsoSection pmap; pmap.ident = kPsoPMAP; pmap.parsed = true;
    PsoSection psch; psch.ident = kPsoPSCH; psch.parsed = true;
    p.sections = {psin, pmap, psch};
    return write_pso(p);
}

// ---------------------------------------------------------------------------------------------------------------
// dat151

std::string interior_dat151_file_name(const std::string& archetype) {
    std::string k;
    for (char c : archetype) if (c != '_') k.push_back((char)tolower((unsigned char)c));
    return k + "_game.dat151.rel";
}

Rel build_interior_dat151(const std::string& name, const std::vector<std::string>& rooms, const InteriorAudioSettings& s) {
    auto H = [&](const std::string& n) { return s.case_sensitive_names ? joaat(n) : joaat_lower(n); };
    Rel rel = Rel::make(reltype::Dat151, 45897013); // the version value of the game's own interior files (and rat_apt01's)
    Dat151InteriorSettings is;
    is.flags = s.flags;
    is.walla_sound_set = audio_hash(s.walla_sound_set);
    is.reflections = audio_hash(s.reflections);
    std::vector<std::pair<uint32_t, Dat151InteriorRoom>> items;
    for (size_t i = 1; i < rooms.size(); i++) {
        std::string full = name + "_" + rooms[i];
        const RoomAudioSettings* rs = &s.default_room;
        for (auto& r : s.rooms) if (r.first == rooms[i]) { rs = &r.second; break; }
        Dat151InteriorRoom r;
        r.flags = rs->flags;
        r.room_name = H(rooms[i]);
        r.ambient_zone = audio_hash(rs->ambient_zone);
        r.interior_type = rs->interior_type;
        r.reverb_small = rs->reverb_small; r.reverb_medium = rs->reverb_medium; r.reverb_large = rs->reverb_large;
        r.room_tone_sound = audio_hash(rs->room_tone_sound);
        r.rain_type = rs->rain_type;
        r.exterior_audibility = rs->exterior_audibility; r.room_occlusion_damping = rs->room_occlusion_damping;
        r.non_marked_portal_occlusion = rs->non_marked_portal_occlusion;
        r.distance_from_portal_for_occlusion = rs->distance_from_portal_for_occlusion;
        r.distance_from_portal_fade_distance = rs->distance_from_portal_fade_distance;
        r.weapon_metrics = audio_hash(rs->weapon_metrics);
        r.walla_sound_set = audio_hash(rs->walla_sound_set);
        is.rooms.push_back(H(full));
        items.push_back({H(full), r});
    }
    rel_add(rel, H(name), is);
    for (auto& [h, r] : items) rel_add(rel, h, r);
    return rel;
}

} // namespace rk
