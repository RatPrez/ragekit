#include "ragekit/render/light.h"
#include <algorithm>
#include <cmath>

namespace rk::render {

namespace {

constexpr float kPi = 3.14159265f;

Vec3 point(const Mat4& m, Vec3 p) {   // row vectors: p * M
    return {p.x * m.at(0, 0) + p.y * m.at(1, 0) + p.z * m.at(2, 0) + m.at(3, 0),
            p.x * m.at(0, 1) + p.y * m.at(1, 1) + p.z * m.at(2, 1) + m.at(3, 1),
            p.x * m.at(0, 2) + p.y * m.at(1, 2) + p.z * m.at(2, 2) + m.at(3, 2)};
}
Vec3 axis(const Mat4& m, Vec3 v) {
    Vec3 r{v.x * m.at(0, 0) + v.y * m.at(1, 0) + v.z * m.at(2, 0), v.x * m.at(0, 1) + v.y * m.at(1, 1) + v.z * m.at(2, 1),
           v.x * m.at(0, 2) + v.y * m.at(1, 2) + v.z * m.at(2, 2)};
    const float l = r.length();
    return l > 1e-12f ? r * (1 / l) : v;
}
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

/// A repeatable value in 0..1 for a light and a time slot.
float noise(uint32_t seed, int64_t slot) {
    uint64_t x = (uint64_t)slot * 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xC2B2AE3D27D4EB4Full);
    x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27; x *= 0x94D049BB133111EBull; x ^= x >> 31;
    return (float)(x >> 40) / (float)(1ull << 24);
}
/// Noise blended smoothly between slots of `rate` per second.
float smooth_noise(uint32_t seed, double t, double rate) {
    const double s = t * rate;
    const int64_t i = (int64_t)std::floor(s);
    const float f = (float)(s - (double)i), k = f * f * (3 - 2 * f);
    return noise(seed, i) * (1 - k) + noise(seed, i + 1) * k;
}

} // namespace

Light make_light(const rk::Light& a, const Mat4& m, uint32_t seed) {
    Light l;
    l.type = a.type;
    l.position = point(m, a.position);
    l.direction = axis(m, a.direction);
    l.tangent = axis(m, a.tangent);
    l.colour = {a.colour[0] / 255.0f, a.colour[1] / 255.0f, a.colour[2] / 255.0f};
    l.intensity = a.intensity;
    l.range = a.falloff;
    l.falloff_exponent = a.falloff_exponent;
    l.cone_inner = a.cone_inner;
    l.cone_outer = a.cone_outer;
    l.capsule_length = a.extent.x;
    // the cull plane's normal turns with the entity, not the light (RageAM); its offset is from the light
    l.cull_normal = axis(m, a.culling_plane_normal);
    l.cull_offset = a.culling_plane_offset;
    l.flags = a.flags;
    l.time_flags = a.time_flags;
    l.flashiness = a.flashiness;
    l.corona_size = a.corona_size;
    l.corona_intensity = a.corona_intensity;
    l.corona_z_bias = a.corona_z_bias;
    l.volume_intensity = a.volume_intensity;
    l.volume_size_scale = a.volume_size_scale;
    l.volume_outer_colour = {a.volume_outer_colour[0] / 255.0f, a.volume_outer_colour[1] / 255.0f, a.volume_outer_colour[2] / 255.0f};
    l.volume_outer_intensity = a.volume_outer_intensity;
    l.volume_outer_exponent = a.volume_outer_exponent;
    l.projected_texture = a.projected_texture_hash;
    l.seed = seed;
    return l;
}

Light transformed(const Light& in, const Mat4& m) {
    Light l = in;
    l.position = point(m, in.position);
    l.direction = axis(m, in.direction);
    l.tangent = axis(m, in.tangent);
    l.cull_normal = axis(m, in.cull_normal);
    return l;
}

bool on_at(const Light& l, float hour) {
    if (l.has(LightIgnoreTime) || (l.time_flags & 0xFFFFFF) == 0) return true;
    float h = std::fmod(hour, 24.0f);
    if (h < 0) h += 24;
    return (l.time_flags >> std::min(23, (int)h)) & 1;
}

float flash(const Light& l, double t) {
    const uint32_t s = l.seed * 2654435761u + 17;
    switch (l.flashiness) {
        case 0: case 2: case 8: case 10: case 18: return 1;                         // constant, when wet, when raining, threshold
        case 1: return noise(s, (int64_t)std::floor(t * 12)) < 0.12f ? 0.35f : 1.0f;  // random dips
        case 3: return std::fmod(t, 1.0) < 0.5 ? 1.0f : 0.0f;                         // once a second
        case 4: return std::fmod(t * 2, 1.0) < 0.5 ? 1.0f : 0.0f;                     // twice
        case 5: return std::fmod(t * 5, 1.0) < 0.5 ? 1.0f : 0.0f;                     // five times
        case 6: return noise(s, (int64_t)std::floor(t * 4)) < 0.5f ? 1.0f : 0.0f;     // random on and off
        case 7: return 0;                                                            // off (the game drives traffic lights)
        case 9: return 0.5f + 0.5f * std::sin((float)t * 2 * kPi);                    // alarm
        case 11: case 12: case 13: {                                                 // slow cycles
            const float period = (float)(l.flashiness - 10);
            return 0.5f + 0.5f * std::sin((float)t * 2 * kPi / period);
        }
        case 14: return noise(s, (int64_t)std::floor(t * 8));                         // disco
        case 15: return 0.82f + 0.18f * smooth_noise(s, t, 9);                        // candle
        case 16: return std::fmod(t, 1.0) < 0.08 ? 1.0f : 0.0f;                       // plane: a short blink each second
        case 17: return 0.6f + 0.4f * smooth_noise(s, t, 6);                          // fire
        case 19: return noise(s, (int64_t)std::floor(t * 30)) < 0.2f ? 0.4f : 1.0f;   // electric buzzing
        case 20: return std::fmod(t * 8, 1.0) < 0.15 ? 1.0f : 0.0f;                   // strobe
        default: return 1;
    }
}

LightConstants light_constants(const Light& l, float range_mult, float intensity_mult) {
    LightConstants c;
    const float range = std::max(l.range * range_mult, 1e-3f);
    Vec3 pos = l.position;
    float extent = 0;
    if (l.type == LightType::Capsule) {
        extent = std::max(l.capsule_length, 0.0f);
        pos = l.position - l.direction * (extent * 0.5f);   // the segment pos .. pos + dir * extent, centred on the light
    }
    c.position = {pos.x, pos.y, pos.z, 1 / (range * range)};
    c.direction = {l.direction.x, l.direction.y, l.direction.z, l.falloff_exponent};
    const float i = l.intensity * intensity_mult;
    c.colour = {l.colour.x * i, l.colour.y * i, l.colour.z * i, extent};
    float scale = 0, offset = 1;
    if (l.type == LightType::Spot) {
        const float inner = std::clamp(l.cone_inner, 1.0f, 89.0f) * kPi / 180;
        const float outer = std::clamp(std::max(l.cone_outer, l.cone_inner), 1.0f, 89.0f) * kPi / 180;
        const float ci = std::cos(inner), co = std::cos(outer);
        scale = 1 / std::max(ci - co, 1e-4f);
        offset = -co * scale;
    }
    const float interior = (l.flags & LightInteriorOnly ? 1.f : 0.f) + (l.flags & LightExteriorOnly ? 2.f : 0.f);
    c.cone = {scale, offset, l.has(LightNoSpecular) ? 0.f : 1.f, interior};
    if (l.has(LightUseCullPlane)) {
        const Vec3& n = l.cull_normal;
        c.cull_plane = {n.x, n.y, n.z, l.cull_offset - dot(n, l.position)};
    } else {
        c.cull_plane = {0, 0, 0, 1};
    }
    return c;
}

std::vector<Light> lights_of(const std::vector<rk::Light>& attrs, const std::function<Mat4(uint16_t)>& bone_matrix) {
    std::vector<Light> out;
    out.reserve(attrs.size());
    for (size_t i = 0; i < attrs.size(); i++) out.push_back(make_light(attrs[i], bone_matrix(attrs[i].bone_id), (uint32_t)i + 1));
    return out;
}

} // namespace rk::render
