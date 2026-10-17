#include "ragekit/render/environment.h"
#include <cmath>

namespace rk::render {

Environment Environment::preset(std::string_view name) {
    Environment e;
    auto set = [&](Vec3 sun, Vec3 sky, Vec3 ground, float exposure) {
        e.sun_colour = sun; e.sky_colour = sky; e.ground_colour = ground; e.exposure = exposure;
    };
    if (name == "overcast")      set({0.45f, 0.47f, 0.5f}, {0.75f, 0.78f, 0.82f}, {0.3f, 0.3f, 0.29f}, 1.0f);
    else if (name == "evening")  set({1.9f, 1.15f, 0.6f}, {0.25f, 0.25f, 0.38f}, {0.12f, 0.09f, 0.07f}, 1.1f);
    else if (name == "night")    set({0.12f, 0.14f, 0.2f}, {0.05f, 0.06f, 0.09f}, {0.03f, 0.03f, 0.03f}, 1.4f);
    else if (name == "interior") set({1.4f, 1.2f, 0.95f}, {0.3f, 0.27f, 0.24f}, {0.16f, 0.14f, 0.12f}, 1.1f);
    else if (name == "studio")   set({1.8f, 1.8f, 1.8f}, {0.5f, 0.5f, 0.5f}, {0.3f, 0.3f, 0.3f}, 1.0f);
    else                         set({2.3f, 2.1f, 1.8f}, {0.42f, 0.48f, 0.6f}, {0.16f, 0.14f, 0.12f}, 1.0f);
    return e;
}

void Environment::set_sun(float azimuth_deg, float elevation_deg) {
    const float a = azimuth_deg * 3.14159265f / 180, el = elevation_deg * 3.14159265f / 180;
    sun_direction = {std::cos(el) * std::sin(a), std::cos(el) * std::cos(a), std::sin(el)};
}

} // namespace rk::render
