#include "ragekit/materials.h"
#include <cstdlib>
#include <sstream>
#include "ragekit/error.h"
#include "ragekit/gameindex.h"

namespace rk {

namespace {

const char* const kNames[] = {
#include "material_names.inc"
};

bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

}  // namespace

int CollisionMaterials::find(std::string_view name) const {
    for (size_t i = 0; i < materials.size(); i++) if (same_name(materials[i].name, name)) return (int)i;
    return -1;
}

CollisionMaterials read_materials_dat(std::string_view text) {
    CollisionMaterials m;
    std::istringstream in{std::string(text)};
    std::string line;
    size_t number = 0;
    while (std::getline(in, line)) {
        number++;
        std::istringstream ls(line);
        std::vector<std::string> t;
        for (std::string w; ls >> w;) t.push_back(w);
        if (t.empty() || t[0][0] == '#') continue;
        if (m.version.empty()) { m.version = t[0]; continue; }
        if (t.size() < 23) throw Error("materials.dat: line " + std::to_string(number) + " has " + std::to_string(t.size()) + " columns, not 23");
        CollisionMaterial c;
        c.name = t[0]; c.filter = t[1]; c.fx_group = t[2]; c.vfx_disturbance = t[3]; c.rumble_profile = t[4]; c.react_weapon = t[5];
        float* f[] = {&c.friction, &c.elasticity, &c.density, &c.tyre_grip, &c.wet_grip, &c.tyre_drag, &c.top_speed_mult, &c.softness, &c.noisiness,
                      &c.penetration_resistance};
        for (size_t i = 0; i < 10; i++) *f[i] = std::strtof(t[6 + i].c_str(), nullptr);
        bool* b[] = {&c.see_thru, &c.shoot_thru, &c.shoot_thru_fx, &c.no_decal, &c.porous, &c.heats_tyre};
        for (size_t i = 0; i < 6; i++) *b[i] = t[16 + i] != "0";
        c.material = t[22];
        m.materials.push_back(std::move(c));
    }
    if (m.version.empty()) throw Error("materials.dat: no version line");
    return m;
}

CollisionMaterials load_materials(const GameIndex& gi) {
    auto f = gi.resolve_file("materials", ".dat");
    if (!f) throw Error("materials.dat: the install has none");
    const Bytes b = gi.extract(*f);
    return read_materials_dat(std::string_view((const char*)b.data(), b.size()));
}

const char* material_name(uint32_t index) { return index < material_count() ? kNames[index] : ""; }

int material_index(std::string_view name) {
    for (uint32_t i = 0; i < material_count(); i++) if (same_name(kNames[i], name)) return (int)i;
    return -1;
}

uint32_t material_count() { return (uint32_t)(sizeof kNames / sizeof kNames[0]); }

}  // namespace rk
