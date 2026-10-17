// ragekit render - the light a scene is drawn in: one sun, sky / ground ambient, exposure. Game space: Z up.
// The presets are rat-tooling's (shared/ui/gta/shading.js LIGHTS), tuned by eye to the game's timecycles; the game's own
// reflection cube map is stood in for by the sky / ground colours.
#pragma once
#include <string_view>
#include "ragekit/types.h"

namespace rk::render {

struct Environment {
    Vec3 sun_direction{0.4f, -0.45f, 0.8f};   // towards the sun, normalised by the backend
    Vec3 sun_colour{2.3f, 2.1f, 1.8f};        // linear, intensity included
    Vec3 sky_colour{0.42f, 0.48f, 0.6f};      // ambient from above
    Vec3 ground_colour{0.16f, 0.14f, 0.12f};  // ambient from below
    float exposure = 1;
    float env_strength = 1;                   // the reflection term
    bool vertex_ao = true;                    // COLOR0.r as ambient occlusion

    /// "day", "overcast", "evening", "night", "interior", "studio"; day for anything else.
    static Environment preset(std::string_view name);
    /// The sun from an azimuth (degrees from +Y, turning towards +X) and elevation (degrees above the horizon).
    void set_sun(float azimuth_deg, float elevation_deg);
};

/// What a backend shows instead of the lit colour.
enum class DebugView : int { Lit = 0, Albedo, Normals, NormalMap, SpecMap, UvCheck, Colour0, Colour1, Alpha };

} // namespace rk::render
