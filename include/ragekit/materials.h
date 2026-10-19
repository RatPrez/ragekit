// ragekit - collision materials: materials.dat (common.rpf/data/materials/materials.dat), the table whose row a bound's
// material index (BoundMaterial::type, Bound::material_index) names.
//
// The file is text: its version ("12.00"), comment lines starting with '#', and one line per material of 23 columns
// (CodeWalker's BoundsMaterialData names them). The row's position is the index the collision files store.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rk {

class GameIndex;

struct CollisionMaterial {
    std::string name, filter, fx_group, vfx_disturbance, rumble_profile, react_weapon;   // "-" where the file has none
    float friction = 0, elasticity = 0, density = 0, tyre_grip = 0, wet_grip = 0, tyre_drag = 0, top_speed_mult = 0;
    float softness = 0, noisiness = 0, penetration_resistance = 0;
    bool see_thru = false, shoot_thru = false, shoot_thru_fx = false, no_decal = false, porous = false, heats_tyre = false;
    std::string material;   // the last column: the name again for most, the audio's for some (PAVING_SLAB -> PAVING_SLABS)
};

struct CollisionMaterials {
    std::string version;
    std::vector<CollisionMaterial> materials;   // by index

    /// The index of a name (any case), -1 when there is none.
    int find(std::string_view name) const;
};

/// Parses materials.dat. Throws rk::Error for a material line without its 23 columns.
CollisionMaterials read_materials_dat(std::string_view text);
/// The install's own materials.dat through the game index.
CollisionMaterials load_materials(const GameIndex& gi);

/// A material's name by index from the built-in table of the game's 213 (the same in both editions); "" past its end.
const char* material_name(uint32_t index);
/// The index of a built-in material name (any case), -1 when there is none.
int material_index(std::string_view name);
/// How many materials the built-in table holds.
uint32_t material_count();

}  // namespace rk
