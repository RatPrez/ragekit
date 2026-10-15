// rk_build_mlo <out.ytyp> [out.ymap]: builds the small test interior described by
// tests/oracle/data/rk_test_mlo.ytyp.xml (one MLO, two rooms, one portal, one entity) with the
// typed API. The oracle converts the XML with CodeWalker and compares it with this file.
// The optional ymap places the interior and a prop and adds a car generator.
#include <cstdio>
#include "ragekit/ragekit.h"
using namespace rk;

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: rk_build_mlo <out.ytyp>\n"); return 2; }
    MapTypes t;
    t.name = joaat_lower("rk_test_mlo");

    Archetype mlo;
    mlo.kind = Archetype::Kind::Mlo;
    mlo.lod_dist = 60;
    mlo.bb_min = {-5, -5, -1};
    mlo.bb_max = {5, 5, 3};
    mlo.bs_centre = {0, 0, 1};
    mlo.bs_radius = 8.125f;
    mlo.hd_texture_dist = 5;
    mlo.name = joaat_lower("rk_test_mlo");
    mlo.asset_type = 4; // ASSET_TYPE_ASSETLESS
    mlo.asset_name = mlo.name;

    Entity box;
    box.archetype_name = joaat_lower("prop_cs_cardbox_01");
    box.flags = 32;
    box.position = {1.5f, 2.25f, 0.5f};
    box.lod_dist = 100;
    box.lod_level = 5; // ORPHANHD
    mlo.entities.push_back(box);

    MloRoom limbo;
    limbo.name = "limbo";
    limbo.bb_min = {-5, -5, -1};
    limbo.bb_max = {5, 5, 3};
    limbo.flags = 96;
    limbo.portal_count = 1;
    MloRoom room;
    room.name = "room_a";
    room.bb_min = {-4, -4, -1};
    room.bb_max = {4, 4, 3};
    room.timecycle_name = joaat_lower("int_nomul");
    room.flags = 96;
    room.portal_count = 1;
    room.attached_objects = {0};
    mlo.rooms = {limbo, room};

    MloPortal portal;
    portal.room_from = 1;
    portal.room_to = 0;
    portal.corners = {{-1, -4, -1}, {-1, -4, 2}, {1, -4, 2}, {1, -4, -1}};
    mlo.portals.push_back(portal);

    t.archetypes.push_back(mlo);
    rsc::write_file(argv[1], write_ytyp(t));
    std::printf("wrote %s\n", argv[1]);

    if (argc > 2) {
        MapData m;
        m.name = joaat_lower("rk_test_map");
        m.flags = 0;
        m.content_flags = 1; // has HD entities
        m.streaming_extents_min = {95, 195, 0};
        m.streaming_extents_max = {115, 215, 10};
        m.entities_extents_min = {99, 199, 0};
        m.entities_extents_max = {111, 211, 5};

        Entity inst;
        inst.is_mlo_instance = true;
        inst.archetype_name = mlo.name;
        inst.position = {105, 205, 2};
        inst.lod_dist = 150;
        inst.lod_level = 5;
        inst.flags = 0;
        inst.default_entity_sets = {joaat_lower("rk_set")};
        inst.group_id = 0;
        inst.floor_id = 0;
        m.entities.push_back(inst);

        Entity prop;
        prop.archetype_name = joaat_lower("prop_cs_cardbox_01");
        prop.position = {101, 201, 1};
        prop.rotation = {0, 0, 0.7071068f, 0.7071068f};
        prop.lod_dist = 100;
        prop.lod_level = 5;
        prop.flags = 1572864;
        m.entities.push_back(prop);

        CarGenerator cg;
        cg.position = {110, 210, 1};
        cg.orient_x = 1; cg.orient_y = 0; cg.perpendicular_length = 2.5f;
        cg.car_model = joaat_lower("sultan");
        cg.flags = 0;
        m.car_generators.push_back(cg);
        m.physics_dictionaries = {joaat_lower("rk_test_phys")};
        m.block.name = "rk_test_map";
        m.block.exported_by = "ragekit";
        rsc::write_file(argv[2], write_ymap(m));
        std::printf("wrote %s\n", argv[2]);
    }
    return 0;
}
