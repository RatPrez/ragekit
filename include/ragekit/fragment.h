// ragekit - fragments (.yft): vehicles, breakable props and ped ragdolls. A fragment wraps a drawable with physics: groups,
// children (each with its own drawables: wheels, doors), archetypes, bounds, joints, glass and cloth.
//
// The physics structures keep their bytes as read (`raw`, with the pointers in them cleared), so what ragekit does not model
// is written back unchanged; their typed members (masses, names, drawables, bounds ...) are what gets written over them.
#pragma once
#include <array>
#include <optional>
#include <string>
#include <vector>
#include "drawable.h"
#include "bounds.h"
#include "cloth.h"

namespace rk {

struct FragDrawable : Drawable {
    BoundTransform frag_matrix;
    std::vector<uint64_t> frag_matrices_inds;
    std::vector<BoundTransform> frag_matrices;  // capacity entries
    uint16_t frag_matrices_count = 0;
};

/// A physics group (176 bytes): tuning values; the ones a tool needs are exposed.
struct FragGroup {
    std::array<uint8_t, 176> raw{};
    std::string name() const;
    uint8_t parent_index() const { return raw[0x4D]; }   // 255 = root
    uint8_t child_index() const { return raw[0x4E]; }
    uint8_t child_count() const { return raw[0x4F]; }
    uint8_t glass_window_index() const { return raw[0x52]; }
    uint8_t glass_flags() const { return raw[0x53]; }
    float mass() const;
    float strength() const;
};

struct FragChild {
    std::array<uint8_t, 256> raw{};
    float pristine_mass = 0, damaged_mass = 0;
    uint16_t group_index = 0, bone_tag = 0;
    std::optional<FragDrawable> drawable;         // pristine
    std::optional<FragDrawable> drawable_damaged;
    std::optional<std::array<uint8_t, 48>> event_set;
    float unk_float = 0;                          // per child values the LOD keeps in parallel arrays
    Vec4 unk_vec, inertia_tensor;
};

struct FragArchetype {
    std::array<uint8_t, 224> raw{};
    std::string name;
    BoundPtr bound;
    float mass() const;
    Vec3 inertia_tensor() const;
};

/// A ped's ragdoll: joints of one degree of freedom (176 bytes) or three (240 bytes; byte 0x15 is 1).
struct FragArticulatedBody {
    std::array<uint8_t, 176> raw{};
    std::vector<Bytes> joints;
    std::vector<Vec4> vectors;
};

struct FragPhysicsLod {
    std::array<uint8_t, 304> raw{};
    std::vector<FragGroup> groups;
    std::vector<FragChild> children;
    std::optional<FragArchetype> archetype, archetype_damaged;
    BoundPtr bound;                               // normally the same bound as archetype's
    std::vector<Mat4> transforms;                 // one per child
    std::optional<FragArticulatedBody> articulated_body;
    Bytes unknown_data1, unknown_data2;
    Vec3 position_offset() const;
};

struct Fragment {
    std::array<uint8_t, 304> raw{};
    std::string name;
    Vec3 sphere_center;
    float sphere_radius = 0;
    std::optional<FragDrawable> drawable;
    std::vector<FragDrawable> drawable_array;
    std::vector<std::string> drawable_array_names;
    std::vector<std::array<float, 12>> bone_transforms;   // 3 rows of 4 per bone
    uint16_t bone_transforms_unk = 0;
    std::vector<std::array<uint8_t, 112>> glass_windows;   // as Legacy stores them; Enhanced's 320 byte declaration is converted to and from the 16 at 0x40
    std::vector<std::array<uint8_t, 320>> glass_declarations_gen9;   // Enhanced: each window's declaration as read, written back while it gives the window's layout
    std::array<std::optional<FragPhysicsLod>, 3> physics;
    std::optional<FragDrawable> drawable_cloth;
    Bytes vehicle_glass_windows;                           // a self-contained block ("VGWH"; offsets inside are relative)
    std::vector<Light> lights;
    std::vector<EnvironmentCloth> cloths;                  // each draws through drawable_cloth if set, else drawable
    int32_t unkB0 = 0, unkB8 = 0, unkBC = 0, unkC0 = 0, unkC4 = 0;
    float unkCC = 0, gravity_factor = 1, buoyancy_factor = 1;

    /// Every drawable to draw for the pristine object, with its placement: the main drawable
    /// at identity, each physics child (LOD 1) at its bone's bind matrix.
    struct Part { const Drawable* drawable; Mat4 transform; std::string name; };
    std::vector<Part> visual_parts() const;
};

/// .yft of either edition (version 162 Legacy, 171 Enhanced).
Fragment read_yft(std::span<const uint8_t> file);
/// A .yft for an edition (game.h); an Enhanced write needs what write_ydr does for every drawable.
Bytes write_yft(const Fragment& f, Game game = default_game());

} // namespace rk
