// ragekit render - the light a scene is drawn in, as the game's lighting shaders take it: the sun, the natural (sky) and
// artificial (interior / exterior) ambient, the local lights' time of day, and the exposure and tone map of postfx.fxc.
// Game space: Z up. Colours are linear with their intensity applied.
//
// Timecycle (render/timecycle.h) fills one from the game's weather and hour; the presets need no game files.
#pragma once
#include <string_view>
#include "ragekit/types.h"

namespace rk::render {

/// The sky (sky_system.fxc's constants, from the timecycle's sky_*): colours linear, before `hdr`.
struct Sky {
    Vec3 azimuth_east{0.5f, 0.6f, 0.8f}, azimuth_west{0.5f, 0.6f, 0.8f}, azimuth_transition{0.5f, 0.6f, 0.8f};
    float azimuth_transition_position = 0.5f;  // 0 east (+X) .. 1 west, along sqrt(0.5 - 0.5 x)
    Vec3 zenith{0.1f, 0.2f, 0.4f}, zenith_transition{0.3f, 0.4f, 0.6f};
    float zenith_transition_position = 0.3f;   // the height (|z| of the direction) the zenith blend starts at
    float zenith_east_blend = 0.8f, zenith_west_blend = 0.8f, zenith_blend_start = 0;
    float hdr = 1;                             // sky_hdr
    Vec3 sun_colour;                           // the sun's scattering colour: sky_sun_col * sky_sun_hdr
    float mie_phase = 0.95f, mie_scatter = 1, mie_intensity = 0.2f;   // sky_sun_miephase / _miescatter / _mie_intensity_mult
    Vec3 sun_disc;                             // sky_sun_disc_col * sky_sun_hdr
    float sun_disc_size = 2;                   // sky_sun_disc_size (degrees)
    Vec3 moon_colour;                          // sky_moon_col * sky_moon_iten (black: no moon)
    float moon_size = 3;                       // sky_moon_disc_size (degrees)
    float stars = 0;                           // sky_stars_iten * sky_hdr, faded in as the sun sets
    /// A plain sky between a horizon and a zenith colour (HDR already), no sun scattering.
    static Sky plain(Vec3 zenith, Vec3 horizon);
};

/// The fog (the game's globalFogParams, from the timecycle's fog_*): colours linear HDR (times fog_hdr / fog_haze_hdr).
struct Fog {
    float start = 0;                           // fog_start (m)
    float density = 0;                         // fog_density (per 10 km)
    float falloff = 0, base_height = 0;        // fog_falloff (per km of height), fog_base_height (m)
    float alpha = 1;                           // fog_alpha
    Vec3 near_colour, colour, east_colour, moon_colour;   // fog_near_col, fog_col (towards the sun), fog_east_col, fog_moon_col
    float sun_power = 8, moon_power = 18;      // fog_sun_lighting_calc_pow, fog_moon_lighting_calc_pow
    float haze_density = 0, haze_alpha = 1, haze_start = 0;   // fog_haze_*
    Vec3 haze_colour;
};

/// Hable's filmic curve as postfx.fxc's PS_Composite takes it (BrightTonemapParams / DarkTonemapParams).
struct Tonemap {
    float a = 0.22f, b = 0.3f, c = 0.1f, d = 0.2f, e = 0.01f, f = 0.3f, w = 4.0f;
};

struct Environment {
    // ---- the sun (gDirectionalLight / gDirectionalColour): the moon at night
    Vec3 sun_direction{0.4f, -0.45f, 0.8f};   // towards the sun, normalised by the backend
    Vec3 sun_colour{50, 46, 40};              // light_dir_col * light_dir_mult

    // ---- ambient (gLightNaturalAmbient0/1, gLightArtificial{Int,Ext}Ambient0/1, gDirectionalAmbientColour): named, as the
    // timecycle does, by where the light goes: `down` falls on surfaces facing up (the sky's blue by day), `up` on surfaces
    // facing down (the ground's bounce); blended by t = max((N.z + wrap) / (1 + wrap), 0). Natural ambient is scaled by
    // the vertex colour's R (squared), artificial by its G (squared).
    Vec3 natural_down{0.9f, 1.2f, 1.6f}, natural_up{2.6f, 3.0f, 3.8f};
    Vec3 artificial_ext_down{0, 0, 0}, artificial_ext_up{0, 0, 0};
    Vec3 artificial_int_down{0.3f, 0.3f, 0.3f}, artificial_int_up{0.5f, 0.5f, 0.5f};
    float ambient_wrap = 0.3f;                 // light_amb_down_wrap
    Vec3 directional_ambient{0, 0, 0};         // along the sun, times sat(N.L)
    float reflection = 1;                      // scales the spec-weighted reflection term
    // the reflection map (the scene around the camera, two paraboloids); without it the sky stands in
    bool reflections = true;
    float reflection_interior_amb = 2, reflection_exterior_amb = 2;   // reflection_tweak_*_amb: the map's artificial ambient
    float reflection_emissive = 2;             // reflection_tweak_emissive: its emissive
    float reflection_directional = 0.3f;       // reflection_tweak_directional: its sun
    float reflection_hdr = 1;                  // reflection_hdr_mult

    // ---- the backdrop and its reflection
    Sky sky;
    bool fog = true;
    Fog fogs;                                  // fog density 0: none (the presets)
    Vec3 sky_zenith{3.0f, 4.2f, 6.5f}, sky_horizon{5.0f, 5.5f, 6.0f};

    // ---- local lights
    float hour = 13;                           // their time flags
    float light_falloff_mult = 1;              // light_falloff_mult: scales every light's range
    float particle_light_mult = 1, particle_emissive_mult = 1;
    float corona_size = 0.75f, corona_intensity = 0.5f;   // misc.coronas.sizeScaleGlobal / intensityScaleGlobal * sprite_size / brightness
    bool lights = true;                        // draw the models' lights and the extra ones
    bool volumes = true;                       // the volumes of LightDrawVolume lights
    bool coronas = true;
    bool shadows = true;                       // the shadow maps of CAST_SHADOWS point and spot lights
    int shadow_lights = 8;                     // at most this many shadowed lights a frame, nearest the camera first (the shaders' 8)
    bool sun_shadows = true;                   // the sun's cascades
    float sun_shadow_distance = 1;             // dir_shadow_distance_multiplier: scales the cascades' reach
    float volume_intensity = 1;                // scales every light volume (their brightness is not yet checked in game)

    // ---- postfx: exposure (EV, the multiplier is 2^EV) and the tone map
    bool auto_exposure = true;                 // EV from the frame's mean luminance: scale * lum^power + offset
    float exposure = 0;                        // postfx_exposure: added to the automatic EV; the EV itself when not automatic
    float exposure_min = -3.5f, exposure_max = 5;
    float exposure_curve[3] = {-106.2f, 0.007f, 104.7f};   // visualsettings.dat Exposure.curve.scale / power / offset
    // adaptation: the EV moves towards the measured one by |difference| * mult per second, at least min, at most max
    // (visualsettings.dat Adaptation.*); a backend's first frame takes the measured EV at once
    bool adaptation = true;
    float adapt_min = 0.15f, adapt_max = 3, adapt_mult = 1.5f;
    Tonemap tonemap_dark, tonemap_bright;      // blended by EV between the two exposures below
    float tonemap_exposure_dark = 3, tonemap_exposure_bright = -0.5f;
    Vec3 correct_colour{1, 1, 1}, shift_colour{1, 1, 1};   // postfx_correct_col / postfx_shift_col
    float correct_cutoff = 0, shift_cutoff = 0, saturation = 1;
    float gamma = 1 / 2.2f;                    // PS_Composite's final power (the game's brightness setting)
    bool tonemap = true;                       // false: the colour times 2^EV, clamped (no curve, no correction)
    // bloom (PS_CalcBloom0, PS_GaussBlur9x9, PS_Composite): the exposed colour, faded in over a band of its luminance
    // above the threshold, blurred at three sizes and added before the tone map
    bool bloom = true;
    float bloom_threshold = 0.4f;              // postfx_bright_pass_thresh, at least bloom.threshold.min (exposed luminance)
    float bloom_threshold_width = 1;           // postfx_bright_pass_thresh_width: the band it fades in over
    float bloom_intensity = 0.2f;              // postfx_intensity_bloom (at most misc.BloomIntensityClamp)
    float bloom_blend[3] = {1, 2, 0.5f};       // bloom.blurblendmult.small / med / large
    // the vignette (PS_Composite) and the gradient filter (PS_GradientFilter)
    float vignette_intensity = 0, vignette_radius = 10, vignette_contrast = 0;   // postfx_vignetting_*
    Vec3 vignette_colour{0, 0, 0};
    Vec3 gradient_top{1, 1, 1}, gradient_middle{1, 1, 1}, gradient_bottom{1, 1, 1};   // postfx_grad_*_col
    float gradient_midpoint = 0.5f;            // postfx_grad_midpoint

    // SSAO (ssao.fxc's CPQSMix, the game's ssao_type 0): darkens both ambients
    bool ssao = true;
    float ssao_strength = 4;                   // ssao_inten: the occlusion's power
    float ssao_cp = 0.5f, ssao_qs = 1;         // ssao_cp_strength / ssao_qs_strength: times it, for the two passes

    bool interior = false;                     // the artificial interior ambient (the game's interior stencil)
    bool vertex_ao = true;                     // COLOR0.r / .g as the ambient's occlusion

    // ---- what an interior's room and its modifier switch off
    bool render_exterior = true;               // the modifier's render_exterior (0 in Mp_apart_mid) and room flag 256: no sky behind the portals
    bool light_time = true;                    // the lights follow their time flags (false: every light on, whatever the hour)

    /// The constants the CPU sets between the files and the shaders, which are reconstructed (ragekit-viewer docs/lights.md,
    /// "Reconstructed"): each one here at the value the backend uses by default, so a tool can expose them and the numbers
    /// tuned against the game come back. A backend draws with tuned() and applies the lights' part per light (tuned_light).
    struct Tuning {
        float light_intensity = 1;             // every local light's intensity (the attribute's own, times this)
        float light_range = 1;                 // every local light's range (on top of light_falloff_mult)
        float falloff_scale = 1;               // the falloff constant k = falloffExponent * this
        bool light_colour_srgb = false;        // a light's colour bytes decoded as sRGB instead of taken as linear / 255
        float ambient_wrap = -1;               // the wrap of the ambient's down / up blend; < 0: the timecycle's light_amb_down_wrap
        float natural_ambient = 1;             // the natural (sky) ambient
        float artificial_ambient = 1;          // the artificial ambient (interior and exterior)
        float directional_ambient = 1;         // the directional ambient (along the sun)
        float emissive = 1;                    // the emissive term
        float reflection = 1;                  // the reflection term
        float exposure_offset = 0;             // EV added after the curve (and to a fixed exposure)
        float adapt_speed = 1;                 // the adaptation's speed (visualsettings' steps times this)

        bool operator==(const Tuning&) const = default;
    } tuning;

    /// A copy with the tuning's ambient, wrap and reflection folded into the colours (what a backend draws with); the
    /// lights', emissive and exposure parts stay in `tuning` for the backend.
    Environment tuned() const;

    /// "day", "overcast", "evening", "night", "interior", "studio"; day for anything else. Values from the game's
    /// EXTRASUNNY / OVERCAST timecycles at 13:00, 19:00 and 23:00; studio has a fixed exposure.
    static Environment preset(std::string_view name);
    /// The sun from an azimuth (degrees from +Y, turning towards +X) and elevation (degrees above the horizon).
    void set_sun(float azimuth_deg, float elevation_deg);
    /// The sun's path over the day (time.xml's sun_roll; CodeWalker's reading of it): towards the sun at `hour`, the moon at
    /// night.
    static Vec3 sun_at(float hour, float sun_roll_deg = 122, float moon_roll_deg = -122);
    /// The sun itself and the moon at an hour (below the horizon too).
    static Vec3 sun_disc_at(float hour, float sun_roll_deg = 122);
    static Vec3 moon_at(float hour, float moon_roll_deg = -122);
    Vec3 moon_direction{0, 0, -1};             // towards the moon
};

// ---- the shaders' arithmetic on the CPU: what a backend's picture is checked against (the same formulas as the GLSL)

/// The ambient light a surface receives before its albedo and kD (directional.fxc): ((natural(t) + directional ambient
/// sat(N.L)) AO_n + artificial(t) AO_a), t = max((N.z + wrap) / (1 + wrap), 0), AO_n = COLOR0.r^2, AO_a = COLOR0.g^2;
/// the artificial ambient the interior one when `interior`. `e` as the backend draws it (Environment::tuned()).
Vec3 ambient_light(const Environment& e, Vec3 normal, float colour0_r, float colour0_g, bool interior);
/// The exposure (EV) postfx shows a frame of mean luminance `lum` at: scale * lum^power + offset + postfx_exposure, plus
/// the tuning's offset, clamped to the timecycle's min / max; postfx_exposure (+ offset) itself when not automatic.
float exposure_ev(const Environment& e, float lum);
/// One adaptation step: from `current` towards `target` by |difference| * Adaptation.step.size.mult EV a second (times
/// the tuning's speed), clamped to the min / max step sizes, over `dt` seconds, never past the target.
float adapt_ev(const Environment& e, float current, float target, float dt);

/// What a backend shows instead of the lit colour.
enum class DebugView : int { Lit = 0, Albedo, Normals, NormalMap, SpecMap, UvCheck, Colour0, Colour1, Alpha, Reflection, Occlusion };

} // namespace rk::render
