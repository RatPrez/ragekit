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
    /// A plain sky between a horizon and a zenith colour (HDR already), no sun scattering.
    static Sky plain(Vec3 zenith, Vec3 horizon);
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
    float volume_intensity = 1;                // scales every light volume (their brightness is not yet checked in game)

    // ---- postfx: exposure (EV, the multiplier is 2^EV) and the tone map
    bool auto_exposure = true;                 // EV from the frame's mean luminance: scale * lum^power + offset
    float exposure = 0;                        // postfx_exposure: added to the automatic EV; the EV itself when not automatic
    float exposure_min = -3.5f, exposure_max = 5;
    float exposure_curve[3] = {-106.2f, 0.007f, 104.7f};   // visualsettings.dat Exposure.curve.scale / power / offset
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

    /// "day", "overcast", "evening", "night", "interior", "studio"; day for anything else. Values from the game's
    /// EXTRASUNNY / OVERCAST timecycles at 13:00, 19:00 and 23:00; studio has a fixed exposure.
    static Environment preset(std::string_view name);
    /// The sun from an azimuth (degrees from +Y, turning towards +X) and elevation (degrees above the horizon).
    void set_sun(float azimuth_deg, float elevation_deg);
    /// The sun's path over the day (time.xml's sun_roll; CodeWalker's reading of it): towards the sun at `hour`, the moon at
    /// night.
    static Vec3 sun_at(float hour, float sun_roll_deg = 122, float moon_roll_deg = -122);
};

/// What a backend shows instead of the lit colour.
enum class DebugView : int { Lit = 0, Albedo, Normals, NormalMap, SpecMap, UvCheck, Colour0, Colour1, Alpha, Reflection, Occlusion };

} // namespace rk::render
