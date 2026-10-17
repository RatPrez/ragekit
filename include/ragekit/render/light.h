// ragekit render - the game's local lights: the CLightAttr of drawables and fragments placed where they shine, whether they
// are on, and the constants the game's light shaders take.
//
// From the game's shaders (point.fxc, spot.fxc, capsule.fxc PS_*CM_standard; the forward lights of ptfx_model.fxc and
// ptfx_sprite.fxc), with P the lit point and L = position - P:
//   attenuation  a = sat(1 - |L|^2 / range^2);  a / ((1 - k) a + k), k = the falloff exponent
//   spot         * sat(dot(-normalize(L), direction) * cone_scale + cone_offset)
//   capsule      L from the nearest point of the segment position .. position + direction * extent
//   cull plane   * (dot(P, n) + d >= 0)
//   then the surface: (albedo * N.L * kD + spec) * colour * intensity, spec = Blinn-Phong with the material's exponent,
//   (n + 2) / 8 normalisation and fresnel, times 0 for NO_SPECULAR lights.
// What the CPU does on the way (cone angles to scale / offset, the capsule centred on its position, flashiness) is not in
// the shaders and is reconstructed; see ragekit-viewer docs/lights.md.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "ragekit/drawable.h"
#include "ragekit/types.h"

namespace rk::render {

/// CLightAttr flags (the game's LIGHTFLAG_ names, from RageAM's lightattr.h).
enum LightFlag : uint32_t {
    LightInteriorOnly = 1u << 0,
    LightExteriorOnly = 1u << 1,
    LightDontUseInCutscene = 1u << 2,
    LightRenderUnderground = 1u << 3,
    LightIgnoreArtificialState = 1u << 4,
    LightTextureProjection = 1u << 5,
    LightCastShadows = 1u << 6,
    LightCastStaticShadows = 1u << 7,
    LightCastDynamicShadows = 1u << 8,
    LightIgnoreTime = 1u << 9,
    LightEnableBuzzing = 1u << 10,
    LightForceBuzzing = 1u << 11,
    LightDrawVolume = 1u << 12,
    LightNoSpecular = 1u << 13,
    LightBothInteriorAndExterior = 1u << 14,
    LightCoronaOnly = 1u << 15,
    LightNotInReflection = 1u << 16,
    LightOnlyInReflection = 1u << 17,
    LightUseCullPlane = 1u << 18,
    LightUseVolumeOuterColour = 1u << 19,
    LightCastHigherResShadows = 1u << 20,
    LightCastLowResShadows = 1u << 21,
    LightFarLodLight = 1u << 22,
    LightDontLightAlpha = 1u << 23,
};

/// A light where it shines (model space in a Model, world space once placed).
struct Light {
    LightType type = LightType::Point;
    Vec3 position, direction{0, 0, -1}, tangent{1, 0, 0};
    Vec3 colour{1, 1, 1};              // linear 0..1: the attribute's bytes / 255
    float intensity = 1;
    float range = 2.5f;                // the attribute's falloff
    float falloff_exponent = 8;
    float cone_inner = 0, cone_outer = 90;   // spot: half angles in degrees
    float capsule_length = 0;          // capsule: the full length along `direction`, centred on `position`
    Vec3 cull_normal{0, 0, 1};         // LightUseCullPlane: lit where dot(P - position, n) + offset >= 0
    float cull_offset = 0;
    uint32_t flags = 0;
    uint32_t time_flags = 0xFFFFFF;    // bit h: on from hour h to h + 1
    uint8_t flashiness = 0;            // 0 constant, 1 random, 3-5 1/2/5 a second, 7 off, 15 candle, 17 fire, 20 strobe ...
    float corona_size = 0, corona_intensity = 0, corona_z_bias = 0.1f;
    float volume_intensity = 0, volume_size_scale = 1;
    Vec3 volume_outer_colour{1, 1, 1};
    float volume_outer_intensity = 0, volume_outer_exponent = 1;
    uint32_t projected_texture = 0;    // LightTextureProjection: the texture's hash
    uint32_t seed = 0;                 // varies the flashiness patterns between lights

    bool has(LightFlag f) const { return (flags & f) != 0; }
    /// Whether it lights surfaces (not just a corona).
    bool lights_surfaces() const { return !has(LightCoronaOnly) && intensity > 0 && range > 0; }
};

/// An attribute of a drawable or fragment, placed by `placement` (its bone's matrix; row vectors).
Light make_light(const rk::Light& attr, const Mat4& placement, uint32_t seed = 0);
/// The light moved by a row-vector matrix (an entity's transform): position, axes and cull plane.
Light transformed(const Light& l, const Mat4& m);

/// Whether the light is on at an hour (0..24): its time flags unless LightIgnoreTime.
bool on_at(const Light& l, float hour);
/// The brightness its flashiness gives at a time in seconds (0..1); 1 for constant lights. An approximation of the
/// game's patterns (they are CPU code; the names are RageAM's).
float flash(const Light& l, double seconds);

/// One light's constants, laid out as the forward shaders' gLight* arrays (and deferredLightParams):
struct LightConstants {
    Vec4 position;    // xyz (a capsule: its first end), w = 1 / range^2
    Vec4 direction;   // xyz spot / capsule axis, w = falloff exponent
    Vec4 colour;      // xyz colour * intensity, w = capsule extent (0 for points and spots)
    Vec4 cone;        // x cone scale, y cone offset (0, 1 for points and capsules), z specular (0 or 1), w interior bits
    Vec4 cull_plane;  // lit where dot(P, xyz) + w >= 0; (0, 0, 0, 1) when the light has none
};
/// `range_mult`: the timecycle's light_falloff_mult; `intensity_mult`: dimming, flashiness. Cone angles clamp to 1..89
/// degrees (RageAM: the game's limits); scale = 1 / (cos inner - cos outer), offset = -cos outer * scale.
LightConstants light_constants(const Light& l, float range_mult = 1, float intensity_mult = 1);

/// The lights of a drawable or fragment's attributes, each on its bone (`bone_matrix(tag)` gives the bone's matrix).
std::vector<Light> lights_of(const std::vector<rk::Light>& attrs, const std::function<Mat4(uint16_t)>& bone_matrix);

} // namespace rk::render
