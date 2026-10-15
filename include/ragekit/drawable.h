// ragekit - drawables (.ydr), drawable dictionaries (.ydd) and the pieces they share
// with fragments (.yft): shader groups, skeletons, models, geometries, lights.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "types.h"
#include "texture.h"

namespace rk {

struct Bound; // bounds.h

// ---------------------------------------------------------------- shaders

enum class ParamType : uint8_t { Texture = 0, Vector = 1, Array = 2 };

struct ShaderParam {
    uint32_t name_hash = 0;      // joaat of the parameter name ("DiffuseSampler", "specularFactor" ...)
    ParamType type = ParamType::Vector;
    std::string texture;         // Texture params: the texture name ("" for a null texture)
    std::vector<Vec4> values;    // Vector: 1 entry; Array: N entries
    int16_t slot = -1;           // the register byte the game stores (0..255); -1 = computed on write (textures i + 2, vectors 160 up)

    static ShaderParam tex(std::string_view name, const std::string& texture);
    static ShaderParam vec(std::string_view name, Vec4 v);
};

struct Shader {
    uint32_t name_hash = 0;      // shader name hash, e.g. joaat("normal_spec")
    uint32_t sps_hash = 0;       // preset file hash, e.g. joaat("normal_spec.sps")
    uint8_t render_bucket = 0;
    uint16_t unk12 = 32768;
    std::vector<ShaderParam> params;

    uint32_t render_bucket_mask() const { return (1u << render_bucket) | 0xFF00u; }
    const ShaderParam* find(uint32_t name_hash) const;
    const ShaderParam* find(std::string_view name) const;
    ShaderParam* find(std::string_view name);
    /// Name of the texture bound to a sampler parameter, or "" when none.
    std::string texture_of(std::string_view sampler) const;
};

struct ShaderGroup {
    std::optional<TextureDictionary> textures; // the drawable's embedded textures
    std::vector<Shader> shaders;
};

// ---------------------------------------------------------------- vertices

enum class VertexSemantic : int {
    Position = 0, BlendWeights = 1, BlendIndices = 2, Normal = 3, Colour0 = 4, Colour1 = 5,
    TexCoord0 = 6, TexCoord1 = 7, TexCoord2 = 8, TexCoord3 = 9, TexCoord4 = 10, TexCoord5 = 11,
    TexCoord6 = 12, TexCoord7 = 13, Tangent = 14, Binormal = 15
};

enum class VertexComponentType : uint8_t {
    Nothing = 0, Half2 = 1, Float = 2, Half4 = 3, FloatUnk = 4, Float2 = 5, Float3 = 6, Float4 = 7,
    UByte4 = 8, Colour = 9, RGBA8SNorm = 10
};
uint32_t vertex_component_size(VertexComponentType t);

/// The vertex declaration: which semantics a vertex has and how each is stored.
struct VertexLayout {
    uint32_t flags = 0;                    // bit per VertexSemantic
    uint64_t types = 0x7755555555996996;   // 4 bits per semantic (the standard GTAV table)

    static constexpr uint64_t kTypesStandard = 0x7755555555996996; // most drawables
    static constexpr uint64_t kTypesCloth1   = 0x030000000199A006;
    static constexpr uint64_t kTypesCloth2   = 0x0300000001996006;
    static constexpr uint64_t kTypesGlass    = 0x7655555555996996;

    bool has(VertexSemantic s) const { return (flags >> (int)s) & 1; }
    VertexComponentType type_of(VertexSemantic s) const { return (VertexComponentType)((types >> ((int)s * 4)) & 0xF); }
    uint32_t offset_of(VertexSemantic s) const;
    uint16_t stride() const;
    uint8_t count() const;
    /// A standard-table layout with the given semantics.
    static VertexLayout standard(std::initializer_list<VertexSemantic> semantics);
};

struct Geometry {
    VertexLayout layout;
    uint32_t vertex_count = 0;
    Bytes vertices;                    // vertex_count * layout.stride()
    std::vector<uint16_t> indices;     // triangle list
    uint16_t shader_index = 0;         // into ShaderGroup::shaders
    AABB bounds;                       // min/max of the vertices (w unused)
    std::vector<uint16_t> bone_ids;    // skinned geometries: bone indices this geometry uses
    uint16_t vb_flags = 0;             // vertex buffer flags (0, or 1024 on some face morphs)
    bool vb_data2_only = false;        // xml: the file had the vertex data on the buffer's second data pointer only (CodeWalker's XML says <Data2>); not written back

    // Typed access to vertex components (decoded/encoded per the layout's component type).
    Vec4 get(uint32_t vertex, VertexSemantic s) const;
    void set(uint32_t vertex, VertexSemantic s, const Vec4& v);
    Vec3 position(uint32_t v) const { return get(v, VertexSemantic::Position).xyz(); }
    Vec3 normal(uint32_t v) const { return get(v, VertexSemantic::Normal).xyz(); }
    Vec2 uv(uint32_t v, int set = 0) const { auto q = get(v, (VertexSemantic)((int)VertexSemantic::TexCoord0 + set)); return {q.x, q.y}; }
    /// Colours come back as 0..1 floats (r,g,b,a).
    Vec4 colour(uint32_t v, int set = 0) const { return get(v, (VertexSemantic)((int)VertexSemantic::Colour0 + set)); }

    void resize_vertices(uint32_t count);
    void compute_bounds();
};

// ---------------------------------------------------------------- models

struct Model {
    std::vector<Geometry> geometries;
    uint32_t skeleton_binding = 0;   // byte 3: bone index; byte 1: has skin
    uint8_t render_mask = 0xFF;
    uint8_t flags = 0;
    AABB bounds;                     // union of the geometry boxes

    uint8_t bone_index() const { return (uint8_t)(skeleton_binding >> 24); }
    void set_bone_index(uint8_t b) { skeleton_binding = (skeleton_binding & 0x00FFFFFFu) | ((uint32_t)b << 24); }
    bool has_skin() const { return (skeleton_binding >> 8) & 0xFF; }
    void set_has_skin(bool s) { skeleton_binding = (skeleton_binding & 0xFFFF00FFu) | (s ? 0x100u : 0u); }
    void compute_bounds();
};

// ---------------------------------------------------------------- skeleton

struct Bone {
    std::string name;
    Quat rotation;
    Vec3 translation;
    Vec3 scale{1, 1, 1};
    int16_t next_sibling = -1;
    int16_t parent = -1;
    uint16_t flags = 0;
    int16_t index = 0;
    uint16_t tag = 0;
    Vec4 transform_unk;      // column 4 of the skeleton's transform entry (purpose unknown)
};

struct Skeleton {
    std::vector<Bone> bones;
    uint32_t unk1C = 0, unk50 = 0, unk54 = 0, unk58 = 0;
    // Raw tables as read; left empty they are rebuilt from `bones` on write.
    std::vector<Mat4> transforms, transforms_inverse;
    std::vector<int16_t> parent_indices, child_indices;

    /// World-space bind matrix of a bone (walks the parents).
    Mat4 bind_matrix(size_t bone) const;
    const Bone* find_tag(uint16_t tag) const;
};

struct Joints {
    Bytes rotation_limits;    // 0xC0 bytes each
    Bytes translation_limits; // 0x40 bytes each
};

// ---------------------------------------------------------------- lights

enum class LightType : uint8_t { Point = 1, Spot = 2, Capsule = 4 };

struct Light {
    Vec3 position;
    uint8_t colour[3] = {255, 255, 255};
    uint8_t flashiness = 0;
    float intensity = 1;
    uint32_t flags = 0;
    uint16_t bone_id = 0;
    LightType type = LightType::Point;
    uint8_t group_id = 0;
    uint32_t time_flags = 0;
    float falloff = 1, falloff_exponent = 1;
    Vec3 culling_plane_normal;
    float culling_plane_offset = 0;
    uint8_t shadow_blur = 0, unk45 = 0;
    uint16_t unk46 = 0;
    float volume_intensity = 0, volume_size_scale = 0;
    uint8_t volume_outer_colour[3] = {0, 0, 0};
    uint8_t light_hash = 0;
    float volume_outer_intensity = 0, corona_size = 0, volume_outer_exponent = 0;
    uint8_t light_fade = 0, shadow_fade = 0, specular_fade = 0, volumetric_fade = 0;
    float shadow_near_clip = 0, corona_intensity = 0, corona_z_bias = 0;
    Vec3 direction{0, 0, -1}, tangent{1, 0, 0};
    float cone_inner = 0, cone_outer = 0;
    Vec3 extent;
    uint32_t projected_texture_hash = 0;
};

// ---------------------------------------------------------------- drawable

enum class Lod : int { High = 0, Medium = 1, Low = 2, VeryLow = 3 };

struct Drawable {
    std::string name;                        // "" when the file has no name (fragment drawables)
    std::optional<ShaderGroup> shader_group;
    std::optional<Skeleton> skeleton;
    std::optional<Joints> joints;
    Vec3 bounding_center;
    float bounding_radius = 0;
    Vec3 bbox_min, bbox_max;
    std::array<std::vector<Model>, 4> lods;  // by Lod
    std::vector<Model> lods_extra;           // the rare fifth list some files carry
    std::array<float, 4> lod_distance{9998, 9998, 9998, 9998};
    std::array<uint32_t, 4> render_mask_flags{};
    std::vector<Light> lights;
    std::shared_ptr<Bound> bound;            // embedded collision, if any
    uint32_t unk3C = 0x7f800001, unk4C = 0x7f800001;

    std::vector<Model>& models(Lod l) { return lods[(int)l]; }
    const std::vector<Model>& models(Lod l) const { return lods[(int)l]; }
    uint8_t lod_flags(Lod l) const { return (uint8_t)(render_mask_flags[(int)l] & 0xFF); }
    uint8_t lod_render_mask(Lod l) const { return (uint8_t)((render_mask_flags[(int)l] >> 8) & 0xFF); }
    /// Recomputes bounding box/sphere, model boxes and render masks from the geometry.
    void compute_bounds();
    /// All models across LODs.
    std::vector<const Model*> all_models() const;
};

struct DrawableDictionary {
    std::vector<uint32_t> hashes;    // parallel to drawables; the game keys by joaat(name)
    std::vector<Drawable> drawables;
    const Drawable* find(uint32_t hash) const;
};

Drawable read_ydr(std::span<const uint8_t> file);
Bytes write_ydr(const Drawable& d);
DrawableDictionary read_ydd(std::span<const uint8_t> file);
Bytes write_ydd(const DrawableDictionary& dd);

/// Tangents (and Binormal) of the vertices from `first_vertex` on: dP/dU across the normal, w -1 where the UVs are mirrored.
void compute_tangents(Geometry& g, size_t first_vertex = 0);

} // namespace rk
