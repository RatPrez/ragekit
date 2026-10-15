// make_interior: a small interior from scratch - an MLO archetype with a limbo room, one room, a portal between them and a
// prop inside (.ytyp), and a map file that places it in the world (.ymap).
//   make_interior <out dir>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <ragekit/ragekit.h>

using namespace rk;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: make_interior <out dir>\n");
        return 2;
    }
    const std::string out = argv[1];
    std::filesystem::create_directories(out);

    Archetype mlo;
    mlo.kind = Archetype::Kind::Mlo;
    mlo.name = mlo.asset_name = joaat_lower("example_interior");
    mlo.asset_type = 4;                   // assetless: the rooms' contents are the entities below
    mlo.lod_dist = 60;
    mlo.bb_min = {-5, -5, -1};
    mlo.bb_max = {5, 5, 3};
    mlo.bs_centre = {0, 0, 1};
    mlo.bs_radius = 7.5f;

    Entity box;                           // positions are relative to the interior
    box.archetype_name = joaat_lower("prop_cs_cardbox_01");
    box.position = {1.5f, 2.25f, 0.5f};
    box.lod_dist = 100;
    box.lod_level = 5;                    // ORPHANHD
    mlo.entities.push_back(box);

    MloRoom limbo;                        // room 0 is always the outside ("limbo")
    limbo.name = "limbo";
    limbo.bb_min = mlo.bb_min;
    limbo.bb_max = mlo.bb_max;
    limbo.portal_count = 1;
    MloRoom room;
    room.name = "room_a";
    room.bb_min = {-4, -4, -1};
    room.bb_max = {4, 4, 3};
    room.timecycle_name = joaat_lower("int_nomul");
    room.flags = 96;
    room.portal_count = 1;
    room.attached_objects = {0};          // indices into mlo.entities
    mlo.rooms = {limbo, room};

    MloPortal door;                       // an opening from room 1 to the outside, four corners in interior space
    door.room_from = 1;
    door.room_to = 0;
    door.corners = {{-1, -4, -1}, {-1, -4, 2}, {1, -4, 2}, {1, -4, -1}};
    mlo.portals.push_back(door);

    MapTypes types;
    types.name = joaat_lower("example_interior");
    types.archetypes.push_back(mlo);
    rsc::write_file(out + "/example_interior.ytyp", write_ytyp(types));

    const Vec3 at{100, 200, 30};
    const float yaw = 0.5f;               // radians about Z
    Entity instance;
    instance.is_mlo_instance = true;
    instance.archetype_name = mlo.name;
    instance.position = at;
    instance.rotation = {0, 0, std::sin(yaw / 2), std::cos(yaw / 2)};   // an MLO instance stores its own orientation
    instance.flags = 1572864;             // what the game's own interior instances carry
    instance.lod_dist = 150;
    instance.lod_level = 5;
    instance.num_exit_portals = 1;

    MapData map;
    map.name = joaat_lower("example_interior_milo_");
    map.content_flags = 1 | 8;            // HD entities, an interior (add 64 when the map lists physics dictionaries)
    map.entities_extents_min = {at.x - 8, at.y - 8, at.z - 2};
    map.entities_extents_max = {at.x + 8, at.y + 8, at.z + 4};
    map.streaming_extents_min = {at.x - 158, at.y - 158, at.z - 152};
    map.streaming_extents_max = {at.x + 158, at.y + 158, at.z + 154};
    map.entities.push_back(instance);
    rsc::write_file(out + "/example_interior_milo_.ymap", write_ymap(map));

    const MapTypes back = read_ytyp(rsc::read_file(out + "/example_interior.ytyp"));
    std::printf("wrote %s: %zu archetype, %zu rooms, %zu portal\n", (out + "/example_interior.ytyp").c_str(), back.archetypes.size(),
                back.archetypes[0].rooms.size(), back.archetypes[0].portals.size());
}
