// ragekit render - a drawable as a backend uploads it: vertex buffers described attribute by attribute, the textures
// its materials sample (found through the drawable's own dictionary, the archetype's and the gtxd parents), and the
// materials themselves. Nothing here touches a GPU; a backend uploads a Model as it is.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "ragekit/drawable.h"
#include "ragekit/fragment.h"
#include "ragekit/texture.h"
#include "ragekit/types.h"
#include "ragekit/render/light.h"
#include "ragekit/render/material.h"

namespace rk {
class GameIndex;
}

namespace rk::render {

/// How one vertex attribute is stored, in terms any graphics API has.
enum class AttribFormat : uint8_t {
    Float1, Float2, Float3, Float4,
    Half2, Half4,
    UNorm8x4,   // 4 bytes read as 0..1 (colours)
    UInt8x4,    // 4 bytes read as integers (blend indices)
    SNorm8x4,   // 4 signed bytes read as -1..1
};

/// The attributes a backend binds, by location.
enum class Attrib : uint8_t { Position, Normal, Tangent, Colour0, Colour1, TexCoord0, TexCoord1, BlendWeights, BlendIndices, Count };

struct VertexAttrib {
    Attrib attrib = Attrib::Position;
    AttribFormat format = AttribFormat::Float3;
    uint32_t offset = 0;      // bytes into the vertex
};

/// One geometry: an indexed triangle list with one material.
struct Mesh {
    std::vector<VertexAttrib> attribs;
    uint32_t stride = 0;
    uint32_t vertex_count = 0;
    std::span<const uint8_t> vertices;   // into Model::sources
    std::span<const uint16_t> indices;
    uint32_t material = 0;               // into Model::materials
    Mat4 transform;                      // placement inside the model (fragment parts), row vectors as the game's
    Vec3 bounds_min, bounds_max;         // model space, before transform
    /// Not drawn in the colour pass: a proxy model (render mask bit 0 clear, as CodeWalker reads it) or a shadow-only
    /// material (FeatShadowOnly). Kept for shadow passes.
    bool shadow_only = false;
    /// The model's render mask: bit 0 the colour pass, bit 1 the shadow passes (a tree's model 0xFD draws without a
    /// shadow, its proxy 0xE2 casts one unseen; most are 0xFF).
    uint8_t render_mask = 0xFF;
    bool casts_shadow() const { return (render_mask & 2) != 0; }
};

/// How a texture's texels are stored, in terms any graphics API has.
enum class PixelFormat : uint8_t {
    Unknown,
    BC1, BC2, BC3, BC4, BC5, BC7,
    RGBA8,        // bytes R, G, B, A
    BGRA8,        // bytes B, G, R, A (D3D9 A8R8G8B8)
    BGRX8,        // as BGRA8, alpha 1
    BGR5A1,       // 16 bits: B 5, G 5, R 5, A 1 (D3D9 A1R5G5B5)
    A8,           // sampled as (0, 0, 0, a), as the game does
    L8,           // sampled as (l, l, l, 1)
};
bool pixel_format_is_block(PixelFormat f);

struct TextureLevel {
    uint32_t width = 0, height = 0;
    std::span<const uint8_t> data;
};

/// A texture as a backend uploads it. Texels are what the game samples: no sRGB decode (the shaders do the gamma).
struct TextureData {
    std::string name;
    uint32_t hash = 0;
    PixelFormat format = PixelFormat::Unknown;
    uint32_t width = 0, height = 0;
    std::vector<TextureLevel> levels;    // into Model::sources
};

/// The texture data of a game Texture (Unknown format when the format has no counterpart).
TextureData texture_data(const Texture& t);

/// A drawable ready to upload.
struct Model {
    std::string name;
    std::vector<Material> materials;
    std::vector<Mesh> meshes;
    std::vector<TextureData> textures;   // every texture a material names that was found, by hash
    std::vector<std::string> missing;    // texture names a material samples that no dictionary had
    std::vector<Light> lights;           // the drawables' light attributes on their bones (model space)
    Vec3 bounds_min, bounds_max;         // model space, transforms applied
    /// What the spans point into: the drawable(s) and the dictionaries the textures came from.
    std::vector<std::shared_ptr<const void>> sources;

    const TextureData* texture(uint32_t hash) const;
};

/// Texture dictionaries a model's textures are looked up in, nearest first.
using TextureDictionaries = std::vector<std::shared_ptr<const TextureDictionary>>;

/// A drawable at one LOD (falling back on the next lower LOD that has models). The drawable's own dictionary is
/// searched before `txds`.
Model build_model(std::shared_ptr<const Drawable> d, const TextureDictionaries& txds = {}, Lod lod = Lod::High);
/// Several drawables drawn together, each with its own shaders (a ped's components).
Model build_model(const std::vector<std::shared_ptr<const Drawable>>& parts, const TextureDictionaries& txds = {}, Lod lod = Lod::High);
/// A fragment at one LOD: the main drawable's models on their bones in the fragment's default pose, the physics
/// children at their LOD 1 transforms, vehicle wheels copied to every wheel.
Model build_model(std::shared_ptr<const Fragment> f, const TextureDictionaries& txds = {}, Lod lod = Lod::High);

/// What load_archetype found.
struct ArchetypeModel {
    Model model;
    std::string drawable_path;           // the file the drawable came from
    std::vector<std::string> txd_names;  // the dictionaries searched after the drawable's own, nearest first
};

/// An archetype (or a bare .ydr / .yft name) of a game install, with its textures: the drawable's own, the archetype's
/// texture dictionary, then that dictionary's parents (gtxd, vehicles.meta). A vehicle ("adder", "adder_hi") also falls
/// back on vehshare; a ped ("a_m_y_hipster_01": a .ydd of components) draws variation 000 of each component.
/// None if neither the archetype nor a drawable is known.
std::optional<ArchetypeModel> load_archetype(const GameIndex& gi, const std::string& name, Lod lod = Lod::High);

} // namespace rk::render
