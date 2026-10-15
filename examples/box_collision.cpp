// box_collision: a collision file (.ybn) made from a triangle mesh - a 4 x 3 x 2.5 m box, closed on all sides.
//   box_collision <out.ybn>
#include <cstdio>
#include <ragekit/ragekit.h>

using namespace rk;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: box_collision <out.ybn>\n");
        return 2;
    }
    const float x = 2, y = 1.5f, z = 2.5f;
    const std::vector<Vec3> v = {{-x, -y, 0}, {x, -y, 0}, {x, y, 0}, {-x, y, 0}, {-x, -y, z}, {x, -y, z}, {x, y, z}, {-x, y, z}};
    const std::vector<uint32_t> tris = {0, 2, 1, 0, 3, 2,   4, 5, 6, 4, 6, 7,   0, 1, 5, 0, 5, 4,
                                        1, 2, 6, 1, 6, 5,   2, 3, 7, 2, 7, 6,   3, 0, 4, 3, 4, 7};   // outward facing
    BoundMaterial concrete;
    concrete.set_type(1);                 // material index in the game's materials.dat

    BoundChild child;
    child.bound = Bound::make_mesh(v, tris, concrete);   // a BVH geometry: vertices, polygons, octants and tree computed
    child.flags1 = 0x3E;                  // what static map collision is (type flags) ...
    child.flags2 = 0x7F3BEC0;             // ... and what it collides with (include flags)
    const BoundPtr ybn = Bound::make_composite({child});
    rsc::write_file(argv[1], write_ybn(*ybn));

    const BoundPtr back = read_ybn(rsc::read_file(argv[1]));
    std::printf("wrote %s: box (%.1f %.1f %.1f) .. (%.1f %.1f %.1f)\n", argv[1], back->box_min.x, back->box_min.y, back->box_min.z, back->box_max.x,
                back->box_max.y, back->box_max.z);
}
