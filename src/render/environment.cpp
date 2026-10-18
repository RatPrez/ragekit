#include "ragekit/render/environment.h"
#include <algorithm>
#include <cmath>

namespace rk::render {

namespace {
constexpr float kPi = 3.14159265f;
Vec3 rotate_x(Vec3 v, float a) { return {v.x, v.y * std::cos(a) - v.z * std::sin(a), v.y * std::sin(a) + v.z * std::cos(a)}; }
} // namespace

Vec3 Environment::sun_disc_at(float hour, float sun_roll_deg) {
    float h = std::fmod(hour, 24.0f);
    if (h < 0) h += 24;
    const float day = kPi * (0.5f + (h - 6) / 14);
    return rotate_x({std::sin(day), -std::cos(day), 0}, sun_roll_deg * kPi / 180);
}

Vec3 Environment::moon_at(float hour, float moon_roll_deg) {
    float h = std::fmod(hour, 24.0f);
    if (h < 0) h += 24;
    const float night = kPi * ((h > 12 ? h - 7 : h + 17) / 9);
    return rotate_x({-std::sin(night), 0, -std::cos(night)}, -moon_roll_deg * kPi / 180);
}

Vec3 Environment::sun_at(float hour, float sun_roll_deg, float moon_roll_deg) {
    float h = std::fmod(hour, 24.0f);
    if (h < 0) h += 24;
    Vec3 d = (h < 5 || h > 21) ? moon_at(h, moon_roll_deg) : sun_disc_at(h, sun_roll_deg);
    if (d.z < 0) d.z = 0;   // the light stays at or above the horizon
    const float l = d.length();
    return l > 0 ? d * (1 / l) : Vec3{0, 0, 1};
}

Sky Sky::plain(Vec3 zenith, Vec3 horizon) {
    Sky s;
    s.azimuth_east = s.azimuth_west = s.azimuth_transition = s.zenith_transition = horizon;
    s.zenith = zenith;
    s.zenith_transition_position = 0;
    s.zenith_east_blend = s.zenith_west_blend = 0;
    s.hdr = 1;
    s.sun_colour = s.sun_disc = {0, 0, 0};
    return s;
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
    // bloom: threshold, width, intensity (EXTRASUNNY's at those hours)
    if (name == "evening") { e.bloom_threshold = 0.798f; e.bloom_threshold_width = 0.598f; e.bloom_intensity = 0.4f; }
    else if (name == "night" || name == "interior") { e.bloom_threshold = 0.4f; e.bloom_threshold_width = 0.25f; e.bloom_intensity = 1; }
    else { e.bloom_threshold = 1.698f; e.bloom_threshold_width = 1.445f; e.bloom_intensity = 0.125f; }
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
    e.sky = Sky::plain(e.sky_zenith, e.sky_horizon);
    return e;
}

Environment Environment::tuned() const {
    Environment e = *this;
    const Tuning& t = tuning;
    e.natural_down = natural_down * t.natural_ambient;
    e.natural_up = natural_up * t.natural_ambient;
    e.artificial_int_down = artificial_int_down * t.artificial_ambient;
    e.artificial_int_up = artificial_int_up * t.artificial_ambient;
    e.artificial_ext_down = artificial_ext_down * t.artificial_ambient;
    e.artificial_ext_up = artificial_ext_up * t.artificial_ambient;
    e.directional_ambient = directional_ambient * t.directional_ambient;
    if (t.ambient_wrap >= 0) e.ambient_wrap = t.ambient_wrap;
    e.reflection = reflection * t.reflection;
    return e;
}

Vec3 ambient_light(const Environment& e, Vec3 n, float r, float g, bool interior) {
    const float len = n.length();
    if (len > 0) n = n * (1 / len);
    const float t = std::max((n.z + e.ambient_wrap) / (1 + e.ambient_wrap), 0.0f);
    auto mix = [](Vec3 a, Vec3 b, float k) { return a * (1 - k) + b * k; };
    Vec3 sd = e.sun_direction;
    const float sl = sd.length();
    if (sl > 0) sd = sd * (1 / sl);
    const float ndl = std::clamp(n.x * sd.x + n.y * sd.y + n.z * sd.z, 0.0f, 1.0f);
    const float ao_n = e.vertex_ao ? r * r : 1, ao_a = e.vertex_ao ? g * g : 1;
    const Vec3 art_down = interior ? e.artificial_int_down : e.artificial_ext_down;
    const Vec3 art_up = interior ? e.artificial_int_up : e.artificial_ext_up;
    return (mix(e.natural_up, e.natural_down, t) + e.directional_ambient * ndl) * ao_n + mix(art_up, art_down, t) * ao_a;
}

float exposure_ev(const Environment& e, float lum) {
    if (!e.auto_exposure) return e.exposure + e.tuning.exposure_offset;
    const float ev = e.exposure_curve[0] * std::pow(std::max(lum, 1e-6f), e.exposure_curve[1]) + e.exposure_curve[2] + e.exposure + e.tuning.exposure_offset;
    return std::clamp(ev, e.exposure_min, e.exposure_max);
}

float adapt_ev(const Environment& e, float current, float target, float dt) {
    const float k = e.tuning.adapt_speed;
    const float diff = target - current;
    const float step = std::clamp(std::abs(diff) * e.adapt_mult * k, e.adapt_min * k, e.adapt_max * k) * std::max(dt, 0.0f);
    return current + std::clamp(diff, -step, step);
}

void Environment::set_sun(float azimuth_deg, float elevation_deg) {
    const float a = azimuth_deg * kPi / 180, el = elevation_deg * kPi / 180;
    sun_direction = {std::cos(el) * std::sin(a), std::cos(el) * std::cos(a), std::sin(el)};
}

} // namespace rk::render
