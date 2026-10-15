// rk_audio_test: self-contained checks of rel.h and audio_occlusion.h (no game files needed).
#include <cstdio>
#include <string>
#include "ragekit/ragekit.h"

using namespace rk;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

int main() {
    // the proxy hash of the game's v_apart_midspaz (342.81573, -997.42883, -100) is 2860953274
    check(occlusion_proxy_hash("v_apart_midspaz", Vec3(342.81573f, -997.42883f, -100.f)) == 2860953274u, "proxy hash of v_apart_midspaz");
    check(rel_data_file_key("dlcLuxe_game.dat151.rel") == "dlcluxe", "data file key is cut at the first underscore");
    check(rel_data_file_key("rat_apt01_game.dat151.rel") == "rat", "data file key of rat_apt01 is 'rat' (two rat_ files would clash)");

    check(interior_dat151_file_name("rat_apt01") == "ratapt01_game.dat151.rel" && rel_data_file_key(interior_dat151_file_name("rat_apt01")) == "ratapt01", "unique rel file name");

    // a three room interior: limbo - hall - bath, a door between hall and bath
    AudioOcclusion m;
    m.name = "test_int"; m.position = Vec3(10.f, 20.f, -50.f);
    m.rooms = {"limbo", "rm_hall", "rm_bath"};
    OcclusionPortal p0; p0.room_from = 0; p0.room_to = 1;                  // front door (no entity)
    OcclusionPortal p1; p1.room_from = 1; p1.room_to = 2;
    OcclusionEntity door; door.model = "v_ilev_test_door"; p1.entities.push_back(door);
    m.portals = {p0, p1};
    OcclusionData d = build_occlusion(m);
    check(d.portals.size() == 4, "two non-mirror portals give four portal infos");
    check(!d.nodes.empty(), "paths exist");
    size_t doors = 0;
    for (auto& pi : d.portals) for (auto& e : pi.entities) { doors++; check(e.model_hash == joaat_lower("v_ilev_test_door") && e.is_door, "door entity"); }
    check(doors == 2, "the door sits in both directions of its portal");
    Bytes ymt = write_occlusion_ymt(d);
    OcclusionData back = read_occlusion_ymt(ymt);
    check(back.portals.size() == d.portals.size() && back.nodes.size() == d.nodes.size(), "ymt reads back");
    check(write_occlusion_ymt(back) == ymt, "ymt read -> write is stable");
    Pso pso = read_pso(ymt);
    check(pso.schema.elements.size() == 5 && pso.map.root_id == 5, "5 structure infos, root block 5 (entities, portals, children, nodes, root)");
    // no entities anywhere: no entity block and no entity struct
    OcclusionData plain = build_occlusion([&] { AudioOcclusion a = m; for (auto& p : a.portals) p.entities.clear(); return a; }());
    Pso pp = read_pso(write_occlusion_ymt(plain));
    check(pp.schema.elements.size() == 4 && pp.map.root_id == 4, "without entities: 4 structure infos, root block 4");

    // dat151
    Rel rel = build_interior_dat151(m);
    check(rel.index.size() == 3 && rel.validate().empty(), "dat151 has settings + 2 rooms");
    Bytes rb = write_rel(rel);
    Rel r2 = read_rel(rb);
    check(write_rel(r2) == rb, "rel read -> write is byte-identical");
    RelItem it;
    check(r2.find(joaat_lower("test_int"), it) && it.type_id == dat151::InteriorSettings, "InteriorSettings found by name hash");
    Dat151InteriorSettings is = Dat151InteriorSettings::read(it);
    check(is.rooms.size() == 2 && is.rooms[0] == joaat_lower("test_int_rm_hall"), "settings list the rooms");
    bool refs = true;
    for (size_t i = 0; i < is.rooms.size(); i++) { uint32_t h = 0; refs = refs && r2.hash_at(r2.hash_table[i], h) && h == is.rooms[i]; }
    check(refs, "the hash table points at the room hash fields");
    RelItem room;
    check(r2.find(joaat_lower("test_int_rm_bath"), room) && Dat151InteriorRoom::read(room).room_name == joaat_lower("rm_bath"), "room item");
    // index order: hashes rotated right by 8 ascending
    bool sorted = true;
    for (size_t i = 1; i < r2.index.size(); i++) {
        auto rot = [](uint32_t h) { return (h >> 8) | (h << 24); };
        sorted = sorted && rot(r2.index[i - 1].hash) < rot(r2.index[i].hash);
    }
    check(sorted, "index sorted like the game's");
    check(rel_type_name(r2.type) == std::string("dat151"), "type name");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
