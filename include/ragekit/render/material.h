// ragekit render - how a drawable's shader draws: the material a backend needs, with no graphics API in it.
//
// The game's shaders share one lighting model (gamma 2 albedo, Blinn-Phong with the game's exponent remap and fresnel,
// ambient by COLOR0 AO, a specular-weighted reflection). What differs per shader is where the material inputs come
// from: which spec map channels give intensity and gloss, the normal map, tint and paint, the vehicles' and weapons'
// "sheen" highlight, emissive masks, alpha. make_material reads that from the shader's name and parameters into
// switches and constants a backend's uber-shader consumes; DrawParams carries what the engine sets per entity.
//
// The formulas come from the game's disassembled Legacy .fxc (rat-tooling research/drawable-shaders.md, sections 1-5).
// Enhanced shaders work the same: Shader::params is the Legacy view of either edition.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "ragekit/drawable.h"
#include "ragekit/types.h"

namespace rk::render {

/// The game's render buckets (the preset's bucket; Shader::render_bucket).
enum class Bucket : uint8_t { Opaque = 0, Alpha = 1, Decal = 2, Cutout = 3, NoSplash = 4, Water = 5, Other6 = 6, Other7 = 7 };

/// What a material's texture slot holds. A backend binds each role to a fixed texture unit.
enum class TextureRole : uint8_t {
    Diffuse, Normal, Specular, Detail, TintPalette, Environment,
    Diffuse2,     // vehicle livery / sign (DiffuseSampler2)
    Dirt,         // vehicle dirt (DirtSampler: R dirt mask, G spec loss)
    Palette,      // weapon / ped diffuse palette (diffuse alpha 32..160 picks the column)
    // terrain: Diffuse and Normal are layer 0
    Layer1, Layer2, Layer3,   // TextureSampler_layer1..3
    Bump1, Bump2, Bump3,      // BumpSampler_layer1..3
    Lookup,       // terrain blend weights (lookupSampler, uv1) in place of COLOR1
    Count
};
const char* texture_role_name(TextureRole r);

/// Which shader family a material belongs to.
enum class Family : uint8_t { Megashader = 0, Vehicle = 1, Weapon = 2, Ped = 3, Hair = 4, Terrain = 5, Unsupported = 255 };

/// Specular intensity: I = sat(base · spec.x [· COLOR0.r]).
enum class SpecIntensity : uint8_t {
    None = 0,      // no spec, and so no reflection
    Param = 1,     // base 1: the multiplier alone
    MapR2 = 2,     // spec map r²
    MapMask = 3,   // dot((r², g², b), specMapIntMask)
    Luminance = 4, // 1 - lum(diffuse) (emissive_speclum)
};
/// Gloss before the exponent remap: raw = base · spec.y.
enum class SpecGloss : uint8_t { Param = 0, MapA = 1, MapG2 = 2, Luminance = 3 };
enum class FresnelSource : uint8_t { Param = 0, OneMinusSpecB = 1 };

/// The second, sun-direction highlight vehicles and weapons write into the albedo (it ignores N·L).
enum class Sheen : uint8_t {
    None = 0,
    Paint = 1,     // si·c.r·0.75 · pow(N·H, s.a·15) · specular2Color
    Paint4 = 2,    // si·c.r·specular2ColorIntensity · pow(N·H, s.a·specular2Factor) · specular2Color
    Mesh = 3,      // s.r²·0.6·c.r · pow(N·H, s.g²·20), white
    Tyre = 4,      // s.r²·1.5·c.r · pow(N·H', s.g²·7), white; H' leans up
    Weapon = 5,    // s.r²·specularFresnel·specular2ColorIntensity · pow(N·H, s.g²·specular2Factor) · specular2Color
};

/// Emissive strength e; the colour adds albedo·e.
enum class Emissive : uint8_t {
    None = 0,
    Mask = 1,          // diffuse.a · emissiveMultiplier · COLOR0.b (props)
    VehicleOpaque = 2, // COLOR0.r · emissiveMultiplier
    DashOpaque = 3,    // COLOR0.r · 0.25
    VehicleLights = 4, // COLOR0.r · lights · emissiveMultiplier · N·V
    DashLights = 5,    // COLOR0.r · lights · 0.3 · N·V
    AlphaMult = 6,     // diffuse.a · emissiveMultiplier (weapon_emissivestrong)
    PedMask = 7,       // diffuse.a · emissiveMultiplier · COLOR0.b, weighted by kD (ped_emissive)
};

enum class AlphaMode : uint8_t {
    Standard = 0,      // diffuse.a · COLOR0.a
    VehicleGlass = 1,  // sat(diffuse.a + window tint.w · COLOR0.a)
    LightId = 2,       // diffuse.a: COLOR0.a is a light id on vehicle light meshes
    Diffuse = 3,       // diffuse.a: every ped vertex shader but ped_default's passes COLOR0.a = 1
    DirtDecal = 4,     // decal_dirt: dot(diffuse.rgb, dirtDecalMask) · COLOR0.a (the mask in MaterialConstants::dirt_colour), albedo · COLOR0.rgb
};

/// Feature switches (Material::features).
enum Feature : uint32_t {
    FeatNormalMap   = 1u << 0,   // BumpSampler with bumpiness
    FeatTint        = 1u << 2,   // *_tnt: albedo · TintPalette(COLOR0.b, the entity's tint row)
    FeatDetail      = 1u << 3,   // *_detail: detailSettings over DetailSampler
    FeatReflect     = 1u << 4,   // *_reflect: albedo + EnvironmentSampler(sphere) · reflectivePower
    FeatUvAnim      = 1u << 5,   // uv = (globalAnimUV0·(u,v,1), globalAnimUV1·(u,v,1))
    FeatWhite       = 1u << 6,   // cpv_only: the diffuse is not applied
    FeatKeepNormal  = 1u << 7,   // decal, decal_tnt, spec_decal: blend colour and spec, keep the surface's normal
    FeatTwoSided    = 1u << 8,   // no back-face culling; N·L from both sides as alpha falls (vehicle glass)
    FeatDeferred    = 1u << 9,   // drawn through the game's deferred path: emissive capped at 16
    FeatShadowOnly  = 1u << 10,  // cpv_only, trees_shadow_proxy, decal_shadow_only: cast shadows, not drawn in colour
                                 // (CodeWalker's reading too; cpv_only is a white stand-in of a tree's trunk and branches)
    FeatPaint       = 1u << 11,  // albedo · matDiffuseColor: a literal colour, or a paint slot (2,n,n) DrawParams fills
    FeatAlbedoR     = 1u << 12,  // albedo · COLOR0.r (vehicles)
    FeatSpecR       = 1u << 13,  // spec intensity · COLOR0.r (vehicles)
    FeatLivery      = 1u << 14,  // DiffuseSampler2 over the albedo by its alpha
    FeatDirt        = 1u << 15,  // DirtSampler mixes dirtColor in by DrawParams::dirt
    FeatPalette     = 1u << 16,  // weapon / ped palette: diffuse alpha 32..160 picks the palette column
    FeatAoSquared   = 1u << 17,  // peds: the ambient occlusion is COLOR0.r² (props use COLOR0.r)
    FeatSkin        = 1u << 18,  // peds: spec map B in [0.8125, 0.9375] marks skin, which gets no reflection
    FeatHairTint    = 1u << 19,  // ped hair: the colour comes from DrawParams::hair, diffuse G the gradient, R the highlight
    FeatLookup      = 1u << 20,  // terrain _cm / _2tex_blend: the layer weights come from the lookup texture (uv1)
};

enum class Blend : uint8_t { None = 0, Alpha = 1, Additive = 2 };

/// The fixed-function state a material draws with.
struct PipelineState {
    Blend blend = Blend::None;
    bool depth_write = true;
    bool cull_back = true;
    bool polygon_offset = false;   // decals: pulled towards the camera
    bool alpha_test = false;       // cut-out: discard alpha <= alpha_ref
    bool alpha_to_coverage = false;
    float alpha_ref = 0.50196f;    // gAlphaTestRef is engine-set; 128/255 as the ped shaders hard-code
};

/// The material constants, laid out for a std140 uniform block (every member a vec4 / ivec4).
struct MaterialConstants {
    Vec4 spec{0.125f, 100, 0.97f, 1};     // intensity multiplier, gloss multiplier, fresnel, bumpiness
    Vec4 spec_mask{1, 0, 0, 1};           // specMapIntMask (xyz), emissiveMultiplier (w)
    Vec4 detail{0, 0, 1, 1};              // detailSettings: x diffuse strength, y bump strength, zw uv scale
    Vec4 uv_anim0{1, 0, 0, 0};            // globalAnimUV0 (xyz)
    Vec4 uv_anim1{0, 1, 0, 0};            // globalAnimUV1 (xyz)
    Vec4 misc{0, 0.50196f, 0, 0};         // x reflectivePower, y alpha ref, z sheen intensity, w sheen exponent factor
    Vec4 tiling{1, 1, 0, 0};              // x diffuse uv scale (DiffuseTexTileUV), y spec uv scale (specTexTileUV)
    Vec4 colour{1, 1, 1, 0};              // matDiffuseColor as a literal colour (rgb); w: paint slot 1..7, 0 = literal
    Vec4 dirt_colour{0.27f, 0.25f, 0.22f, 0};   // dirtColor (gamma space)
    int32_t modes[4]{0, 0, 0, 0};         // features, SpecIntensity, SpecGloss, FresnelSource
    int32_t modes2[4]{0, 0, 0, 0};        // Sheen, Emissive, AlphaMode, Bucket
    int32_t uv_sets[4]{1, 0, -1, 0};      // UV set of the livery, of the spec map, of the dirt (-1 none); Family
    Vec4 hair{0.1f, 0.15f, 16, 32};       // anisotropicSpecularIntensity (xy), anisotropicSpecularExponent (zw)
    Vec4 hair_colour{0.1f, 0.1f, 0.1f, 0}; // anisotropicSpecularColour
};
static_assert(sizeof(MaterialConstants) == 224, "std140 block");

/// A texture a material samples: the name the shader parameter gives (hash: joaat_lower of it).
struct TextureSlot {
    std::string name;
    uint32_t hash = 0;
    bool used() const { return hash != 0; }
};

struct Material {
    std::string shader;          // "normal_spec" (the base shader name), "hash_XXXXXXXX" when unknown
    uint32_t shader_hash = 0;
    Family family = Family::Megashader;
    Bucket bucket = Bucket::Opaque;
    uint32_t features = 0;
    SpecIntensity spec = SpecIntensity::None;
    SpecGloss gloss = SpecGloss::Param;
    FresnelSource fresnel = FresnelSource::Param;
    Sheen sheen = Sheen::None;
    Emissive emissive = Emissive::None;
    AlphaMode alpha = AlphaMode::Standard;
    PipelineState state;
    MaterialConstants constants;
    std::array<TextureSlot, (size_t)TextureRole::Count> textures;

    const TextureSlot& texture(TextureRole r) const { return textures[(size_t)r]; }
    bool has(Feature f) const { return (features & f) != 0; }
};

/// The material a drawable's shader draws with.
Material make_material(const Shader& s);

/// What the engine sets per entity, which a drawable's files do not hold.
struct DrawParams {
    int tint = 0;                  // *_tnt: the entity's tint (palette row)
    int palette = 0;               // weapon palette row (the weapon tint)
    /// Vehicle paint for matDiffuseColor (2,n,n), gamma space: [1] primary, [2] secondary, [3] pearlescent, [4] wheels,
    /// [5] white (fixed), [6] interior trim, [7] dashboard.
    std::array<Vec3, 8> paint{Vec3{1, 1, 1}, Vec3{0.11f, 0.31f, 0.70f}, Vec3{0.11f, 0.12f, 0.14f}, Vec3{0.54f, 0.54f, 0.54f},
                              Vec3{0.65f, 0.66f, 0.67f}, Vec3{1, 1, 1}, Vec3{0.1f, 0.1f, 0.1f}, Vec3{0.1f, 0.1f, 0.1f}};
    Vec3 sheen_colour{0.54f, 0.54f, 0.54f};   // specular2Color (the files hold a placeholder): the pearlescent colour
    float dirt = 0;                // 0 clean .. 1
    bool lights = false;           // every vehicle light dimmer on
    Vec4 window_tint{1, 1, 1, 0};  // matDiffuseColorTint: rgb tint, w added opacity where COLOR0.a marks tintable glass
    /// Ped hair colour, gamma space: the dark and light ends of the hair colour's palette row, then of the highlight's.
    /// The game binds a palette texture the drawables do not name; set_hair builds the rows from its 64 colours.
    bool hair_tint = false;
    std::array<Vec3, 4> hair{};

    /// The game's hair colour `colour` (0-63, the character creator's numbering) with highlight `highlight` (-1: none).
    /// The colours are the game's; how far a row spreads around one is an approximation (rat-tooling's).
    void set_hair(int colour, int highlight = -1);
};

/// The name of a shader hash ("normal_spec"), empty if ragekit does not know it.
std::string shader_name(uint32_t hash);

} // namespace rk::render
