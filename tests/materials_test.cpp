// rk_materials_test: the collision material table: the built-in names, the materials.dat parser, and (with an install)
// the install's file against the built-in table.
#include <cstdio>
#include <cstring>
#include <string>
#include "ragekit/ragekit.h"

using namespace rk;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL  %s\n", what.c_str()); failures++; }
}

int main() {
    check(material_count() == 213, "213 built-in materials");
    const std::pair<uint32_t, const char*> known[] = {{0, "DEFAULT"}, {1, "CONCRETE"}, {11, "STONE"}, {13, "BRICK"}, {55, "METAL_SOLID_SMALL"},
                                                      {67, "METAL_GARAGE_DOOR"}, {112, "GLASS_SHOOT_THROUGH"}, {113, "GLASS_BULLETPROOF"}, {212, "TEMP_30"}};
    for (auto& [i, n] : known) {
        check(std::strcmp(material_name(i), n) == 0, "material " + std::to_string(i) + " is " + n + " (" + material_name(i) + ")");
        check(material_index(n) == (int)i, std::string("index of ") + n);
    }
    check(material_index("glass_shoot_through") == 112, "names match in any case");
    check(std::string(material_name(213)).empty() && material_index("NOT_A_MATERIAL") == -1, "past the table");

    const char* text =
        "12.00\n# comment\n\n"
        "DEFAULT\t\tDEFAULT\tGENERIC\tDEFAULT\t-\t-\t1.0\t0.1\t1750.0\t1.00\t-0.15\t0.0\t1.0\t0.95\t0.0\t1.0\t0 0 0 0\t0 0\tDEFAULT\n"
        "GLASS_X\tGLASS\tGLASS\tDEFAULT\t-\tGLASS\t0.5\t0.2\t2500.0\t1.00\t-0.15\t0.0\t1.0\t0.0\t0.5\t0.1\t1 1 0 1\t0 1\tGLASS_Y\n";
    const CollisionMaterials m = read_materials_dat(text);
    check(m.version == "12.00" && m.materials.size() == 2, "the parser reads the version and two lines");
    if (m.materials.size() == 2) {
        const CollisionMaterial& g = m.materials[1];
        check(g.name == "GLASS_X" && g.react_weapon == "GLASS" && g.material == "GLASS_Y", "names of a line");
        check(g.friction == 0.5f && g.density == 2500.f && g.penetration_resistance == 0.1f, "numbers of a line");
        check(g.see_thru && g.shoot_thru && !g.shoot_thru_fx && g.no_decal && !g.porous && g.heats_tyre, "flags of a line");
        check(m.find("glass_x") == 1, "find");
    }
    bool threw = false;
    try { read_materials_dat("12.00\nSHORT LINE 1 2 3\n"); } catch (const Error&) { threw = true; }
    check(threw, "a short line throws");

    // a bound's own material id reads as a BoundMaterial: its flag bytes are the material flags' low and high bytes
    Bound bd;
    bd.material_index = 112; bd.room_ped_density = 3; bd.unk_flags = kMatSeeThrough | kMatShootThrough; bd.poly_flags = kMatNoDecal >> 8; bd.material_colour_index = 7;
    const BoundMaterial bm = bd.material();
    check(bm.type() == 112 && bm.room_id() == 3 && bm.flags() == (kMatSeeThrough | kMatShootThrough | kMatNoDecal) && bm.colour_index() == 7, "Bound::material()");
    Bound be;
    be.set_material(bm);
    check(be.material() == bm && be.unk_flags == bd.unk_flags && be.poly_flags == bd.poly_flags, "Bound::set_material()");

    const std::string dir = GameIndex::default_game_dir();
    if (!dir.empty()) {
        const CollisionMaterials g = load_materials(GameIndex::open(dir));
        check(g.materials.size() == material_count(), "the install has " + std::to_string(g.materials.size()) + " materials");
        for (size_t i = 0; i < g.materials.size() && i < material_count(); i++)
            check(g.materials[i].name == material_name((uint32_t)i), "install material " + std::to_string(i) + " " + g.materials[i].name);
        std::printf("install: %zu materials, version %s\n", g.materials.size(), g.version.c_str());
    } else std::printf("no install: the game's file is not checked\n");
    std::printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
