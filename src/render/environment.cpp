#include "ragekit/render/environment.h"
#include <cmath>

namespace rk::render {

namespace {
constexpr float kPi = 3.14159265f;
Vec3 rotate_x(Vec3 v, float a) { return {v.x, v.y * std::cos(a) - v.z * std::sin(a), v.y * std::sin(a) + v.z * std::cos(a)}; }
} // namespace

Vec3 Environment::sun_at(float hour, float sun_roll_deg, float moon_roll_deg) {
    float h = std::fmod(hour, 24.0f);
    if (h < 0) h += 24;
    const float day = kPi * (0.5f + (h - 6) / 14), night = kPi * ((h > 12 ? h - 7 : h + 17) / 9);
    Vec3 sun = rotate_x({std::sin(day), -std::cos(day), 0}, sun_roll_deg * kPi / 180);
    Vec3 moon = rotate_x({-std::sin(night), 0, -std::cos(night)}, -moon_roll_deg * kPi / 180);
    Vec3 d = (h < 5 || h > 21) ? moon : sun;
    if (d.z < 0) d.z = 0;   // the light stays at or above the horizon
    const float l = d.length();
    return l > 0 ? d * (1 / l) : Vec3{0, 0, 1};
}

Environment Environment::preset(std::string_view name) {
    // the game's timecycle values (GLOBAL region) for each preset, read with Timecycle::environment
    struct P {
        float hour; Vec3 sun, nat_down, nat_up, ext_down, ext_up, int_down, int_up, dir_amb, zenith, horizon;
        float wrap, exposure, exp_max;
    };
    P p;
    if (name == "overcast")   // OVERCAST 13:00
        p = {13, {54.45f, 50.94f, 42.97f}, {5.37f, 5.62f, 5.88f}, {3.76f, 3.63f, 3.35f}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
             {0, 0, 0}, {0.96f, 0.90f, 0.76f}, {9.63f, 17.72f, 22.99f}, {24.41f, 24.41f, 24.41f}, 0.25f, 0, 5};
    else if (name == "evening")   // EXTRASUNNY 19:00
        p = {19, {11.02f, 4.39f, 1.17f}, {0.377f, 0.535f, 0.877f}, {0.187f, 0.207f, 0.243f}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
             {0, 0, 0}, {1.0f, 0.526f, 0.282f}, {0.518f, 0.674f, 1.207f}, {2.619f, 3.809f, 4.813f}, 0.2f, 0, 5};
    else if (name == "night")   // EXTRASUNNY 23:00
        p = {23, {0.042f, 0.09f, 0.2f}, {0.001f, 0.003f, 0.015f}, {0.001f, 0.004f, 0.01f}, {0.1f, 0.023f, 0}, {0.1f, 0.023f, 0.001f},
             {0, 0, 0}, {0, 0, 0}, {0.004f, 0.006f, 0.01f}, {0.004f, 0.006f, 0.011f}, {0.003f, 0.008f, 0.021f}, 0.4f, 0, 5};
    else   // EXTRASUNNY 13:00
        p = {13, {61.09f, 51.87f, 34.83f}, {2.294f, 3.088f, 4.057f}, {3.746f, 3.2f, 2.247f}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
             {0, 0, 0}, {2.0f, 1.71f, 1.16f}, {0.026f, 0.176f, 0.383f}, {0.733f, 2.071f, 4.527f}, 0.312f, 0, 5};
    Environment e;
    e.hour = p.hour;
    e.sun_direction = sun_at(p.hour);
    e.sun_colour = p.sun;
    e.natural_down = p.nat_down; e.natural_up = p.nat_up;
    e.artificial_ext_down = p.ext_down; e.artificial_ext_up = p.ext_up;
    e.artificial_int_down = p.int_down; e.artificial_int_up = p.int_up;
    e.directional_ambient = p.dir_amb;
    e.sky_zenith = p.zenith; e.sky_horizon = p.horizon;
    e.ambient_wrap = p.wrap;
    e.exposure = p.exposure; e.exposure_max = p.exp_max;
    if (name == "interior") {
        // a room lit by its own lights: no sun, the artificial interior ambient, night hours (lights on)
        e.hour = 22;
        e.sun_colour = {0, 0, 0};
        e.natural_down = e.natural_up = {0.02f, 0.02f, 0.025f};
        e.artificial_int_down = {0.4f, 0.37f, 0.33f};   // from the ceiling's lights onto what faces up
        e.artificial_int_up = {0.25f, 0.23f, 0.2f};
        e.directional_ambient = {0, 0, 0};
        e.sky_zenith = e.sky_horizon = {0.02f, 0.02f, 0.022f};
        e.interior = true;
    } else if (name == "studio") {
        // even light from every side and a fixed exposure, for looking at a model
        e.sun_direction = {0.4f, -0.45f, 0.8f};
        e.sun_colour = {3.2f, 3.2f, 3.2f};
        e.natural_down = {1.0f, 1.0f, 1.0f};
        e.natural_up = {0.6f, 0.6f, 0.6f};
        e.directional_ambient = {0, 0, 0};
        e.sky_zenith = e.sky_horizon = {0.25f, 0.26f, 0.28f};
        e.auto_exposure = false;
        e.exposure = 0;
    }
    return e;
}

void Environment::set_sun(float azimuth_deg, float elevation_deg) {
    const float a = azimuth_deg * kPi / 180, el = elevation_deg * kPi / 180;
    sun_direction = {std::cos(el) * std::sin(a), std::cos(el) * std::cos(a), std::sin(el)};
}

} // namespace rk::render
