// ragekit - fragments (.yft): breakable props and vehicles. A fragment wraps a drawable
// with physics: groups, children (each with its own drawable, e.g. wheels, doors),
// archetypes and bounds. Reading is complete for rendering and inspection.
#pragma once
#include <array>
#include <optional>
#include <string>
#include <vector>
#include "drawable.h"
#include "bounds.h"

namespace rk {

/// A drawable inside a fragment: a plain drawable plus the fragment matrices.
struct FragDrawable : Drawable {
    BoundTransform frag_matrix;                 // flags default to 0x7f800001
    std::vector<uint64_t> frag_matrices_inds;
    std::vector<BoundTransform> frag_matrices;  // capacity entries
    uint16_t frag_matrices_count = 0;
};

/// A physics group (176 bytes). Most fields are tuning values; the ones a tool needs are exposed.
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
    float pristine_mass = 0, damaged_mass = 0;
    uint16_t group_index = 0, bone_tag = 0;
    std::optional<FragDrawable> drawable;         // pristine
    std::optional<FragDrawable> drawable_damaged;
    bool has_event_set = false;
    float unk_float = 0;          // per child values the LOD keeps in parallel arrays
    Vec4 unk_vec, inertia_tensor;
};

struct FragArchetype {
    std::array<uint8_t, 224> raw{};
    std::string name;
    BoundPtr bound;
    float mass() const;
    Vec3 inertia_tensor() const;
};

struct FragPhysicsLod {
    std::array<uint8_t, 304> raw{};               // the struct as read; pointers inside are stale
    std::vector<FragGroup> groups;
    std::vector<FragChild> children;
    std::optional<FragArchetype> archetype, archetype_damaged;
    BoundPtr bound;                               // normally the same bound as archetype's
    std::vector<Mat4> transforms;                 // one per child
    Bytes articulated_body;                       // 176 bytes when present (joints not decoded)
    Bytes unknown_data1, unknown_data2;
    Vec3 position_offset() const;
};

struct Fragment {
    std::string name;
    Vec3 sphere_center;
    float sphere_radius = 0;
    std::optional<FragDrawable> drawable;
    std::vector<FragDrawable> drawable_array;
    std::vector<std::string> drawable_array_names;
    std::vector<std::array<float, 12>> bone_transforms;   // 3 rows of 4 per bone
    uint16_t bone_transforms_unk = 0;
    std::vector<std::array<uint8_t, 112>> glass_windows;
    std::array<std::optional<FragPhysicsLod>, 3> physics;
    std::optional<FragDrawable> drawable_cloth;
    Bytes vehicle_glass_windows;                           // raw block, "VGWH"
    std::vector<Light> lights;
    size_t cloth_count = 0;
    int32_t unkB0 = 0, unkB8 = 0, unkBC = 0, unkC0 = 0, unkC4 = 0;
    float unkCC = 0, gravity_factor = 1, buoyancy_factor = 1;

    /// Every drawable to draw for the pristine object, with its placement: the main drawable
    /// at identity, each physics child (LOD 1) at its bone's bind matrix.
    struct Part { const Drawable* drawable; Mat4 transform; std::string name; };
    std::vector<Part> visual_parts() const;
};

Fragment read_yft(std::span<const uint8_t> file);

} // namespace rk
