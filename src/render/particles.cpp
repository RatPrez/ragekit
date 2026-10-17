#include "ragekit/render/particles.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "ragekit/error.h"
#include "ragekit/gameindex.h"
#include "ragekit/hash.h"
#include "ragekit/image.h"

namespace rk::render {

namespace {

using Block = std::shared_ptr<PtxBlock>;
constexpr float kPi = 3.14159265f;
constexpr uint32_t kKfp = 0x90;

// ---------------------------------------------------------------- reading the rules (CodeWalker's Particle.cs offsets)

Block to(const PtxBlock& b, uint32_t at) { auto l = b.link(at); return l ? l->to : nullptr; }

/// What an empty keyframe gives: 1 for the scalars and colours, 100 for the percentages, 0 for the rest.
Vec4 kfp_default(uint32_t name) {
    static const std::map<uint32_t, Vec4> table = [] {
        std::map<uint32_t, Vec4> t;
        auto set = [&](const char* n, Vec4 v) { t[joaat_lower(n)] = v; };
        set("ptxEmitterRule:m_particleLifeKFP", {1, 1, 0, 0});
        set("ptxEmitterRule:m_playbackRateScalarKFP", {1, 1, 1, 1});
        set("ptxEmitterRule:m_speedScalarKFP", {1, 1, 0, 0});
        set("ptxEmitterRule:m_sizeScalarKFP", {100, 100, 100, 100});
        set("ptxEmitterRule:m_accnScalarKFP", {100, 100, 100, 100});
        set("ptxEmitterRule:m_dampeningScalarKFP", {100, 100, 100, 100});
        set("ptxEmitterRule:m_matrixWeightScalarKFP", {100, 100, 100, 100});
        set("ptxEffectRule:m_colourTintMinKFP", {1, 1, 1, 1});
        set("ptxEffectRule:m_colourTintMaxKFP", {1, 1, 1, 1});
        set("ptxEffectRule:m_zoomScalarKFP", {100, 100, 100, 100});
        set("ptxu_Size:m_tblrScalarKFP", {1, 1, 1, 1});
        set("ptxu_Colour:m_rgbaMinKFP", {1, 1, 1, 1});
        set("ptxu_Colour:m_rgbaMaxKFP", {1, 1, 1, 1});
        set("ptxu_MatrixWeight:m_mtxWeightKFP", {1, 1, 0, 0});
        set("ptxu_Light:m_rgbMinKFP", {1, 1, 1, 1});
        set("ptxu_Light:m_rgbMaxKFP", {1, 1, 1, 1});
        set("ptxu_Light:m_coronaRgbMinKFP", {1, 1, 1, 1});
        set("ptxu_Light:m_coronaRgbMaxKFP", {1, 1, 1, 1});
        return t;
    }();
    auto it = table.find(name);
    return it == table.end() ? Vec4{} : it->second;
}

Keyframe read_kfp(const PtxBlock& b, uint32_t at) {
    Keyframe k;
    k.name = b.get<uint32_t>(at + 0x68);
    k.invert_bias = b.get<uint8_t>(at + 0x6C) != 0;
    k.random_index = b.get<uint8_t>(at + 0x6D);
    k.fallback = kfp_default(k.name);
    if (auto v = to(b, at + 0x70))
        for (uint32_t i = 0; i + 0x20 <= v->bytes.size(); i += 0x20) {
            k.times.push_back(v->get<float>(i));
            k.values.push_back({v->get<float>(i + 16), v->get<float>(i + 20), v->get<float>(i + 24), v->get<float>(i + 28)});
        }
    return k;
}

Domain read_domain(const PtxBlock& d) {
    Domain o;
    o.shape = (DomainShape)d.get<uint8_t>(0x0C);
    o.world_space = d.get<uint8_t>(0x10) != 0;
    o.point_relative = d.get<uint8_t>(0x11) != 0;
    o.creation_relative = d.get<uint8_t>(0x12) != 0;
    o.target_relative = d.get<uint8_t>(0x13) != 0;
    o.position = read_kfp(d, 0x18);
    o.rotation = read_kfp(d, 0x18 + kKfp);
    o.size_outer = read_kfp(d, 0x18 + 2 * kKfp);
    o.size_inner = read_kfp(d, 0x18 + 3 * kKfp);
    return o;
}

std::shared_ptr<EmitterRule> read_emitter(const PtxBlock& e) {
    auto r = std::make_shared<EmitterRule>();
    r->name = e.string_at(0x20);
    if (auto d = to(e, 0x38)) r->creation = read_domain(*d);
    if (auto d = to(e, 0x48)) r->target = read_domain(*d);
    if (auto d = to(e, 0x58)) r->attractor = read_domain(*d);
    Keyframe* ks[] = {&r->spawn_rate, &r->spawn_rate_distance, &r->life, &r->playback_rate, &r->speed, &r->size_scale,
                      &r->acceleration_scale, &r->damping_scale, &r->matrix_weight_scale, &r->inherit_velocity};
    for (int i = 0; i < 10; i++) *ks[i] = read_kfp(e, 0x78 + i * kKfp);
    r->one_shot = e.get<uint8_t>(0x628) != 0;
    return r;
}

Alignment alignment(int v) { return v >= 0 && v <= 4 ? (Alignment)v : Alignment::CameraFacing; }

std::shared_ptr<ParticleRule> read_particle(const PtxBlock& p, const ParticleDictionary& dict) {
    auto r = std::make_shared<ParticleRule>();
    r->name = p.string_at(0x120);
    r->shader = p.string_at(0x1B8);
    r->technique = p.string_at(0x1C0);
    r->cull_mode = p.get<int>(0x100);
    r->blend_set = p.get<int>(0x104);
    r->lighting_mode = p.get<int>(0x108);
    r->depth_write = p.get<uint8_t>(0x10C) != 0;
    r->depth_test = p.get<uint8_t>(0x10D) != 0;
    r->alpha_blend = p.get<uint8_t>(0x10E) != 0;
    r->frame_min = p.get<uint32_t>(0x118);
    r->frame_max = p.get<uint32_t>(0x11C);
    r->diffuse_mode = p.get<uint32_t>(0x1E0);
    r->lit = p.get<uint8_t>(0x1E8) != 0;
    r->soft = p.get<uint8_t>(0x1E9) != 0;
    r->screen_space = p.get<uint8_t>(0x1EA) != 0;
    r->refract = p.get<uint8_t>(0x1EB) != 0;
    r->normal_spec = p.get<uint8_t>(0x1EC) != 0;
    r->sort_type = p.get<uint8_t>(0x220);
    r->draw_type = p.get<uint8_t>(0x221);

    // shader variables
    static const uint32_t kDiffuse = joaat_lower("DiffuseTex2");
    if (auto list = to(p, 0x1F0))
        for (uint32_t i = 0; i < list->bytes.size(); i += 8) {
            auto v = to(*list, i);
            if (!v) continue;
            ShaderVar s;
            s.name = v->get<uint32_t>(0x10);
            s.type = v->get<uint8_t>(0x14);
            if (s.type == 2 || s.type == 4) s.value = {v->get<float>(0x30), v->get<float>(0x34), v->get<float>(0x38), v->get<float>(0x3C)};
            else if (s.type == 6) {
                if (!v->textures.empty()) s.texture = v->textures[0].name;
                else s.texture = v->string_at(0x30);
                if (s.name == kDiffuse) r->texture = s.texture;
            } else if (s.type == 7) {
                if (auto items = to(*v, 0x28))
                    for (uint32_t k = 0; k + 0x20 <= items->bytes.size(); k += 0x20) s.keys.emplace_back(items->get<float>(k), items->get<float>(k + 0x10));
            }
            r->vars.push_back(std::move(s));
        }
    // the drawables ptxd_Model picks from
    if (auto d = to(p, 0x210))
        for (uint32_t i = 0; i + 0x30 <= d->bytes.size(); i += 0x30) {
            std::string name = d->string_at(i + 0x10);
            for (auto& dl : d->drawables)
                if (dl.at == i + 0x18 && name.empty()) name = "#" + std::to_string(dl.index);
            r->drawables.push_back(name);
        }
    // bias links
    if (auto bl = to(p, 0x188))
        for (uint32_t i = 0; i + 0x58 <= bl->bytes.size(); i += 0x58) {
            ParticleRule::BiasLink link;
            link.random_index = bl->get<uint8_t>(i + 0x50);
            if (auto ids = bl->link(i + 0x40))
                for (uint32_t k = 0; k + 4 <= ids->to->bytes.size(); k += 4) link.props.push_back(ids->to->get<uint32_t>(k));
            r->bias_links.push_back(std::move(link));
        }

    // behaviours
    auto all = to(p, 0x128);
    if (!all) return r;
    for (uint32_t i = 0; i < all->bytes.size(); i += 8) {
        auto bp = to(*all, i);
        if (!bp) continue;
        const PtxBlock& b = *bp;
        auto k = [&](int n) { return read_kfp(b, 0x30 + n * kKfp); };
        const uint32_t type = b.get<uint32_t>(8);
        switch (type) {
            case 0xF5B33BAA: break;   // ptxu_Age
            case 0xD63D9F1B: r->acceleration = {k(0), k(1), b.get<int>(0x158), b.get<uint8_t>(0x15C) != 0, b.get<uint8_t>(0x15D) != 0}; break;
            case 0x6C0719BC: r->velocity = true; break;
            case 0x1EE64552:
                r->rotation = {k(0), k(1), k(2), k(3), b.get<int>(0x270), b.get<int>(0x274), b.get<uint8_t>(0x278) != 0, b.get<float>(0x27C)};
                break;
            case 0x38B60240: r->size = {k(0), k(1), k(2), k(3), b.get<int>(0x270), b.get<uint8_t>(0x274) != 0}; break;
            case 0x052B1293: r->dampening = {k(0), k(1), b.get<int>(0x158), b.get<uint8_t>(0x15C) != 0}; break;
            case 0x64E5D702: r->matrix_weight = k(0); break;
            case 0x928A1C45: r->collision = {k(0), k(1), b.get<float>(0x150), b.get<float>(0x154), b.get<int>(0x158), b.get<int>(0x15C)}; break;
            case 0xECA84C1E:
                r->animate = {k(0), b.get<int>(0xC0), b.get<int>(0xC4), b.get<int>(0xC8), b.get<uint8_t>(0xCC) != 0,
                              b.get<uint8_t>(0xCD) != 0, b.get<uint8_t>(0xCE) != 0, b.get<uint8_t>(0xCF) != 0};
                break;
            case 0x164AEA72:
                r->colour = {k(0), k(1), k(2), b.get<int>(0x1E0), b.get<uint8_t>(0x1E4) != 0, b.get<uint8_t>(0x1E5) != 0, b.get<uint8_t>(0x1E6) != 0};
                break;
            case 0x68FA73F5: {
                ParticleRule::Sprite s;
                s.axis = {b.get<float>(0x30), b.get<float>(0x34), b.get<float>(0x38)};
                s.align = alignment(b.get<int>(0x40));
                s.flip_u = b.get<float>(0x44);
                s.flip_v = b.get<float>(0x48);
                s.near_clip = b.get<float>(0x4C);
                s.far_clip = b.get<float>(0x50);
                s.projection = b.get<float>(0x54);
                s.screen = b.get<uint8_t>(0x5C) != 0;
                s.disabled = b.get<uint8_t>(0x61) != 0;
                r->sprite = s;
                break;
            }
            case 0x38B63978: r->wind = k(0); break;
            case 0x0544C710: {
                ParticleRule::Light l;
                l.rgb_min = k(0); l.rgb_max = k(1); l.intensity = k(2); l.range = k(3);
                l.corona_rgb_min = k(4); l.corona_rgb_max = k(5); l.corona_intensity = k(6); l.corona_size = k(7); l.corona_flare = k(8);
                l.corona_z_bias = b.get<float>(0x540);
                l.corona_use_light_colour = b.get<uint8_t>(0x544) != 0;
                l.colour_from_particle = b.get<uint8_t>(0x545) != 0;
                l.colour_per_frame = b.get<uint8_t>(0x546) != 0;
                l.intensity_per_frame = b.get<uint8_t>(0x547) != 0;
                l.range_per_frame = b.get<uint8_t>(0x548) != 0;
                l.shadows = b.get<uint8_t>(0x549) != 0;
                l.type = b.get<int>(0x54C);
                r->light = l;
                break;
            }
            case 0x6232E25A: break;   // ptxd_Model: draws the rule's drawables
            case 0xA35C721F: r->zcull = {k(0), k(1), b.get<int>(0x158), b.get<int>(0x15C)}; break;
            case 0xB77FED19: r->noise = {k(0), k(1), k(2), k(3), b.get<int>(0x270), b.get<uint8_t>(0x274) != 0}; break;
            case 0xC57377F8: {
                ParticleRule::Trail t;
                t.tex_info = k(0);
                t.axis = {b.get<float>(0xC0), b.get<float>(0xC4), b.get<float>(0xC8)};
                t.align = alignment(b.get<int>(0xD0));
                t.tess_u = b.get<int>(0xD4);
                t.tess_v = b.get<int>(0xD8);
                t.wrap_over_life = b.get<uint8_t>(0xEE) != 0;
                t.disabled = b.get<uint8_t>(0xEF) != 0;
                r->trail = t;
                break;
            }
            default: r->unsimulated.push_back(hash_name(type)); break;
        }
    }
    (void)dict;
    return r;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }
Vec4 lerp4(Vec4 a, Vec4 b, float t) { return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)}; }
Vec3 xyz(Vec4 v) { return {v.x, v.y, v.z}; }
Vec3 mulv(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 point(const Mat4& m, Vec3 p) {   // row vectors: p * M
    return {p.x * m.at(0, 0) + p.y * m.at(1, 0) + p.z * m.at(2, 0) + m.at(3, 0),
            p.x * m.at(0, 1) + p.y * m.at(1, 1) + p.z * m.at(2, 1) + m.at(3, 1),
            p.x * m.at(0, 2) + p.y * m.at(1, 2) + p.z * m.at(2, 2) + m.at(3, 2)};
}
Vec3 vector(const Mat4& m, Vec3 v) {
    return {v.x * m.at(0, 0) + v.y * m.at(1, 0) + v.z * m.at(2, 0), v.x * m.at(0, 1) + v.y * m.at(1, 1) + v.z * m.at(2, 1),
            v.x * m.at(0, 2) + v.y * m.at(1, 2) + v.z * m.at(2, 2)};
}

/// A domain's rotation (degrees about x, y, z): the y turn first, then x, then z. The game's domains are laid out with their
/// long axis on y and turned 90 degrees about x to stand up (most of the game's emitters).
Vec3 rotate_deg(Vec3 v, Vec3 deg) {
    const float ax = deg.x * kPi / 180, ay = deg.y * kPi / 180, az = deg.z * kPi / 180;
    Vec3 a{v.x * std::cos(ay) + v.z * std::sin(ay), v.y, -v.x * std::sin(ay) + v.z * std::cos(ay)};
    Vec3 b{a.x, a.y * std::cos(ax) - a.z * std::sin(ax), a.y * std::sin(ax) + a.z * std::cos(ax)};
    return {b.x * std::cos(az) - b.y * std::sin(az), b.x * std::sin(az) + b.y * std::cos(az), b.z};
}

/// A repeatable random number in 0..1 for (seed, index).
float hash01(uint32_t seed, uint32_t index) {
    uint64_t x = ((uint64_t)seed << 32) ^ (index * 0x9E3779B9u + 0x7F4A7C15u);
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ull; x ^= x >> 33;
    return (float)(x >> 40) / (float)(1ull << 24);
}

} // namespace

// ---------------------------------------------------------------- keyframes

Vec4 Keyframe::at(float t) const {
    if (values.empty()) return fallback;
    if (values.size() == 1 || t <= times.front()) return values.front();
    if (t >= times.back()) return values.back();
    for (size_t i = 1; i < values.size(); i++)
        if (t <= times[i]) {
            const float span = times[i] - times[i - 1];
            return lerp4(values[i - 1], values[i], span > 0 ? (t - times[i - 1]) / span : 1);
        }
    return values.back();
}

float ShaderVar::at(float t) const {
    if (type != 7) return value.x;
    if (keys.empty()) return 0;
    if (keys.size() == 1 || t <= keys.front().first) return keys.front().second;
    for (size_t i = 1; i < keys.size(); i++)
        if (t <= keys[i].first) {
            const float span = keys[i].first - keys[i - 1].first;
            return lerp(keys[i - 1].second, keys[i].second, span > 0 ? (t - keys[i - 1].first) / span : 1);
        }
    return keys.back().second;
}

const ShaderVar* ParticleRule::var(std::string_view name) const {
    const uint32_t h = joaat_lower(name);
    for (auto& v : vars) if (v.name == h) return &v;
    return nullptr;
}

// ---------------------------------------------------------------- the library

void EffectLibrary::add(std::shared_ptr<const ParticleDictionary> dict) {
    if (!dict) return;
    dicts_.push_back(dict);
    std::map<const PtxBlock*, std::shared_ptr<EmitterRule>> emitters;
    std::map<const PtxBlock*, std::shared_ptr<ParticleRule>> particles;
    auto emitter = [&](const Block& b) -> std::shared_ptr<EmitterRule> {
        if (!b) return nullptr;
        auto& e = emitters[b.get()];
        if (!e) e = read_emitter(*b);
        return e;
    };
    auto particle = [&](const Block& b) -> std::shared_ptr<ParticleRule> {
        if (!b) return nullptr;
        auto& p = particles[b.get()];
        if (!p) {
            p = read_particle(*b, *dict);
            const uint32_t top = std::max<uint32_t>(p->frame_max, p->animate && p->animate->last_frame > 0 ? (uint32_t)p->animate->last_frame : 0);
            if (!p->texture.empty()) {
                auto& m = max_frame_[p->texture];
                m = std::max(m, top);
            }
        }
        return p;
    };
    for (size_t i = 0; i < dict->effect_rules.rules.size(); i++) {
        const Block& eb = dict->effect_rules.rules[i];
        if (!eb) continue;
        auto e = std::make_shared<EffectRule>();
        e->name = eb->string_at(0x20);
        e->loops = eb->get<int>(0x50);
        e->random_offset = {eb->get<float>(0x60), eb->get<float>(0x64), eb->get<float>(0x68)};
        e->preupdate = eb->get<float>(0x70);
        e->preupdate_interval = eb->get<float>(0x74);
        e->duration_min = eb->get<float>(0x78);
        e->duration_max = eb->get<float>(0x7C);
        e->playback_min = eb->get<float>(0x80);
        e->playback_max = eb->get<float>(0x84);
        e->tint_min = read_kfp(*eb, 0xC0);
        e->tint_max = read_kfp(*eb, 0xC0 + kKfp);
        e->zoom = read_kfp(*eb, 0xC0 + 2 * kKfp);
        e->data_sphere = read_kfp(*eb, 0xC0 + 3 * kKfp);
        e->data_capsule = read_kfp(*eb, 0xC0 + 4 * kKfp);
        e->tint_max_enable = eb->get<uint8_t>(0x3A0) != 0;
        if (auto evo = to(*eb, 0x48))
            if (auto names = to(*evo, 0))
                for (uint32_t k = 0; k + 0x18 <= names->bytes.size(); k += 0x18) e->evolutions.push_back(names->string_at(k));
        if (auto list = to(*eb, 0x38))
            for (uint32_t k = 0; k < list->bytes.size(); k += 8) {
                auto ee = to(*list, k);
                if (!ee) continue;
                EventEmitter ev;
                ev.start = ee->get<float>(0x10);
                ev.end = ee->get<float>(0x14);
                ev.emitter = emitter(to(*ee, 0x40));
                ev.particle = particle(to(*ee, 0x48));
                ev.playback_min = ee->get<float>(0x50);
                ev.playback_max = ee->get<float>(0x54);
                ev.zoom_min = ee->get<float>(0x58);
                ev.zoom_max = ee->get<float>(0x5C);
                ev.tint_min = ee->get<uint32_t>(0x60);
                ev.tint_max = ee->get<uint32_t>(0x64);
                if (ev.emitter && ev.particle) e->events.push_back(std::move(ev));
            }
        effects_[e->name] = e;
    }
    for (size_t i = 0; i < dict->drawables.size() && i < dict->drawable_hashes.size(); i++) {
        auto d = std::shared_ptr<const Drawable>(dict, &dict->drawables[i]);
        drawables_[hash_name(dict->drawable_hashes[i])] = d;
        drawables_["#" + std::to_string(i)] = d;
    }
}

std::shared_ptr<const ParticleDictionary> EffectLibrary::load(const GameIndex& gi, std::string_view name) {
    auto vs = gi.versions(joaat_lower(name), ".ypt");
    if (vs.empty()) return nullptr;
    const IndexedFile* pick = nullptr;
    for (const char* q : {"/ptfx_hi.rpf/", "/ptfx.rpf/"}) {
        for (auto it = vs.rbegin(); it != vs.rend() && !pick; ++it)
            if (gi.path(*it).find(q) != std::string::npos) pick = &*it;
        if (pick) break;
    }
    if (!pick) pick = &vs.back();
    return std::make_shared<ParticleDictionary>(read_ypt(gi.extract(*pick)));
}

std::shared_ptr<const EffectRule> EffectLibrary::effect(std::string_view name) const {
    auto it = effects_.find(name);
    return it == effects_.end() ? nullptr : it->second;
}

std::vector<std::string> EffectLibrary::effect_names() const {
    std::vector<std::string> out;
    for (auto& [n, e] : effects_) out.push_back(n);
    return out;
}

const Texture* EffectLibrary::texture(std::string_view name) const {
    for (auto& d : dicts_)
        if (d->textures)
            if (auto t = d->textures->find(name)) return t;
    return nullptr;
}

std::shared_ptr<const Drawable> EffectLibrary::drawable(std::string_view name) const {
    auto it = drawables_.find(name);
    return it == drawables_.end() ? nullptr : it->second;
}

FrameGrid EffectLibrary::frames(std::string_view name) const {
    {
        std::lock_guard<std::mutex> lock(grid_mutex_);
        auto c = grids_.find(name);
        if (c != grids_.end()) return c->second;
    }
    auto it = max_frame_.find(name);
    const uint32_t n = it == max_frame_.end() ? 1 : it->second + 1;
    const Texture* t = texture(name);
    FrameGrid best;
    if (t && n > 1) {
        // The frames sit on empty gutters: score each grid that holds the frames (one fewer too: the files' last frame
        // ids are off by one now and then) by how much of the texture's coverage lies on its cell boundaries.
        uint32_t level = 0;
        while (level + 1 < t->levels && (t->width >> level) > 256) level++;
        Image img = decode_texture(*t, level);
        std::vector<float> cols(img.width, 0), rows(img.height, 0);
        for (uint32_t y = 0; y < img.height; y++)
            for (uint32_t x = 0; x < img.width; x++) {
                const uint8_t* p = &img.rgba[((size_t)y * img.width + x) * 4];
                const float v = (float)p[3] * (float)std::max({p[0], p[1], p[2]}) / (255.0f * 255.0f);
                cols[x] = std::max(cols[x], v);
                rows[y] = std::max(rows[y], v);
            }
        auto mean = [](const std::vector<float>& p) { float s = 0; for (float v : p) s += v; return p.empty() ? 1.0f : std::max(s / (float)p.size(), 1e-4f); };
        auto boundary = [&](const std::vector<float>& p, uint32_t k) {   // the profile at the k cells' edges, against its mean
            if (k <= 1) return 0.0f;
            float s = 0;
            for (uint32_t i = 1; i < k; i++) {
                const float x = (float)i * (float)p.size() / (float)k;
                const int a = std::clamp((int)std::floor(x - 0.5f), 0, (int)p.size() - 1), b = std::clamp((int)std::floor(x + 0.5f), 0, (int)p.size() - 1);
                s += std::min(p[(size_t)a], p[(size_t)b]);
            }
            return s / (float)(k - 1) / mean(p);
        };
        float best_score = 1e30f;
        for (uint32_t c = 1; c <= 16; c++)
            for (uint32_t r = 1; r <= 16; r++) {
                const uint32_t cells = c * r;
                if (cells + 1 < n || cells > std::max(n * 2, n + 8)) continue;
                const float score = boundary(cols, c) + boundary(rows, r) + 0.02f * (float)(cells - std::min(cells, n));
                if (score < best_score - 1e-4f) { best_score = score; best = {c, r}; }
            }
    }
    std::lock_guard<std::mutex> lock(grid_mutex_);
    grids_[std::string(name)] = best;
    return best;
}

// ---------------------------------------------------------------- playing

namespace {

/// A particle's random number for a keyframe: its bias link's when it has one, the keyframe's own otherwise.
float pick(const Particle& p, const ParticleRule& r, const Keyframe& k) {
    uint32_t index = k.random_index;
    for (auto& l : r.bias_links)
        if (std::find(l.props.begin(), l.props.end(), k.name) != l.props.end()) { index = l.random_index; break; }
    const float v = hash01(p.seed, index);
    return k.invert_bias ? 1 - v : v;
}
/// Between a min and a max keyframe at t.
Vec4 range(const Particle& p, const ParticleRule& r, const Keyframe& lo, const Keyframe& hi, float t) {
    return lerp4(lo.at(t), hi.at(t), pick(p, r, lo));
}
/// A keyframe holding min in x and max in y.
float scalar(float r, const Keyframe& k, float t) {
    const Vec4 v = k.at(t);
    return lerp(v.x, v.y, r);
}

/// A point of a domain shape (local, before its rotation): `outer` and `inner` its full sizes.
Vec3 sample_shape(DomainShape s, Vec3 outer, Vec3 inner, std::mt19937& rng) {
    std::uniform_real_distribution<float> u(0, 1);
    switch (s) {
        case DomainShape::Sphere: {
            // a direction, a radius between inner and outer (uniform in volume)
            Vec3 d;
            float l = 0;
            do { d = {u(rng) * 2 - 1, u(rng) * 2 - 1, u(rng) * 2 - 1}; l = d.length(); } while (l > 1 || l < 1e-4f);
            d = d * (1 / l);
            const float ri = std::clamp(std::max({inner.x, inner.y, inner.z}) / std::max({outer.x, outer.y, outer.z, 1e-6f}), 0.0f, 1.0f);
            const float r = std::cbrt(lerp(ri * ri * ri, 1, u(rng)));
            return mulv(d, outer * 0.5f) * r;
        }
        case DomainShape::Cylinder: {
            // the axis is y: a radius between inner and outer in x / z, a height in y
            const float a = u(rng) * 2 * kPi;
            const float ri = std::clamp(std::max(inner.x, inner.z) / std::max({outer.x, outer.z, 1e-6f}), 0.0f, 1.0f);
            const float r = std::sqrt(lerp(ri * ri, 1, u(rng)));
            return {std::cos(a) * r * outer.x * 0.5f, (u(rng) - 0.5f) * outer.y, std::sin(a) * r * outer.z * 0.5f};
        }
        default: {
            // a box: anywhere inside, outside the inner box when there is one
            for (int tries = 0; tries < 16; tries++) {
                Vec3 p{(u(rng) - 0.5f) * outer.x, (u(rng) - 0.5f) * outer.y, (u(rng) - 0.5f) * outer.z};
                if (std::abs(p.x) * 2 >= inner.x || std::abs(p.y) * 2 >= inner.y || std::abs(p.z) * 2 >= inner.z || tries == 15) return p;
            }
            return {};
        }
    }
}

Vec3 sample_domain(const Domain& d, float er, std::mt19937& rng) {
    Vec3 local = sample_shape(d.shape, xyz(d.size_outer.at(er)), xyz(d.size_inner.at(er)), rng);
    return rotate_deg(local, xyz(d.rotation.at(er))) + xyz(d.position.at(er));
}

Vec4 argb(uint32_t c) { return {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f}; }

} // namespace

EffectInstance::EffectInstance(std::shared_ptr<const EffectRule> rule, const EffectLibrary& lib, uint32_t seed)
    : rule_(std::move(rule)), lib_(&lib), rng_(seed), next_seed_(seed * 2654435761u + 1) {
    restart();
}

float EffectInstance::rand() { return std::uniform_real_distribution<float>(0, 1)(rng_); }

void EffectInstance::restart() {
    batches_.clear();
    emitters_.clear();
    time_ = 0;
    loop_ = 0;
    timeline_done_ = false;
    if (!rule_) return;
    duration_ = std::max(0.001f, lerp(rule_->duration_min, rule_->duration_max, rand()));
    playback_ = std::max(0.0f, lerp(rule_->playback_min, rule_->playback_max, rand()));
    const Vec4 z = rule_->zoom.at(0);
    zoom_ = lerp(z.x, z.y, rand()) / 100;
    for (auto& ev : rule_->events) {
        ParticleBatch b;
        b.rule = ev.particle;
        b.grid = lib_->frames(ev.particle->texture);
        batches_.push_back(std::move(b));
        EmitterState s;
        s.rate_r = rand();
        emitters_.push_back(s);
    }
    // ambient effects start established: run the pre-update time in its steps
    if (rule_->preupdate > 0) {
        const float step = rule_->preupdate_interval > 0 ? rule_->preupdate_interval : 0.25f;
        for (float t = 0; t < rule_->preupdate; t += step) update(std::min(step, rule_->preupdate - t));
    }
}

bool EffectInstance::finished() const {
    if (!timeline_done_) return false;
    for (auto& b : batches_) if (!b.particles.empty()) return false;
    return true;
}

size_t EffectInstance::particle_count() const {
    size_t n = 0;
    for (auto& b : batches_) n += b.particles.size();
    return n;
}

void EffectInstance::spawn(size_t event, float er, int count) {
    const EventEmitter& ev = rule_->events[event];
    const EmitterRule& em = *ev.emitter;
    const ParticleRule& pr = *ev.particle;
    ParticleBatch& batch = batches_[event];
    const float zoom = zoom_ * lerp(ev.zoom_min, ev.zoom_max, emitters_[event].rate_r);   // the event's is a scalar (1)
    // the effect's tint: its keyframes (min or between min and max) times the event's
    Vec4 tint = rule_->tint_min.at(time_ / duration_);
    if (rule_->tint_max_enable) tint = lerp4(tint, rule_->tint_max.at(time_ / duration_), rand());
    const Vec4 et = lerp4(argb(ev.tint_min), argb(ev.tint_max), rand());
    tint = {tint.x * et.x, tint.y * et.y, tint.z * et.z, tint.w * et.w};
    const size_t kMaxParticles = 4096;
    for (int k = 0; k < count && batch.particles.size() < kMaxParticles; k++) {
        Particle p;
        p.seed = next_seed_++ * 2246822519u + 3266489917u;
        auto r = [&](const Keyframe& kf) { const float v = hash01(p.seed, kf.random_index); return kf.invert_bias ? 1 - v : v; };
        p.life = std::max(0.01f, scalar(r(em.life), em.life, er));
        p.playback = std::max(0.0f, em.playback_rate.at(er).x);
        p.size_scale = xyz(em.size_scale.at(er)) * (zoom / 100);
        p.tint = tint;
        // where it starts and where it heads, in the effect's space, then into the world
        Vec3 local = em.creation ? sample_domain(*em.creation, er, rng_) : Vec3{};
        local = local * zoom;
        Vec3 vel;
        if (em.target) {
            Vec3 target = sample_domain(*em.target, er, rng_) * zoom;
            if (em.target->point_relative) target = target + local;
            vel = (target - local) * scalar(r(em.speed), em.speed, er);
        }
        if (em.creation && em.creation->world_space) {
            p.position = local + Vec3{transform_.at(3, 0), transform_.at(3, 1), transform_.at(3, 2)};
            p.velocity = vel;
        } else {
            p.position = point(transform_, local);
            p.velocity = vector(transform_, vel);
        }
        if (pr.rotation) {
            p.angles = xyz(range(p, pr, pr.rotation->init_min, pr.rotation->init_max, 0));
            p.angular_speeds = xyz(range(p, pr, pr.rotation->speed_min, pr.rotation->speed_max, 0));
            p.angle = p.angles.x;
            p.angular_speed = p.angular_speeds.x;
        }
        // the frame: random in the rule's range, then animated
        const float span = (float)(pr.frame_max >= pr.frame_min ? pr.frame_max - pr.frame_min : 0);
        p.frame = (float)pr.frame_min + std::floor(hash01(p.seed, 1000) * (span + 1));
        p.frame = std::min(p.frame, (float)pr.frame_max);
        if (pr.animate) {
            if (!pr.animate->random_start) p.frame = (float)pr.frame_min;
            p.frame_rate = scalar(pick(p, pr, pr.animate->rate), pr.animate->rate, 0);
        }
        if (pr.sprite) {
            p.flip_u = hash01(p.seed, 1001) < pr.sprite->flip_u;
            p.flip_v = hash01(p.seed, 1002) < pr.sprite->flip_v;
        }
        batch.particles.push_back(p);
    }
}

void EffectInstance::update(float dt_in) {
    if (!rule_ || dt_in <= 0) return;
    const float dt = dt_in * playback_;
    // ---- the timeline: each event emits while the effect's ratio is inside its range
    if (!timeline_done_) {
        time_ += dt;
        const float ratio = std::min(time_ / duration_, 1.0f);
        for (size_t i = 0; i < rule_->events.size(); i++) {
            const EventEmitter& ev = rule_->events[i];
            EmitterState& st = emitters_[i];
            if (ratio < ev.start || ratio > ev.end) continue;
            const float span = ev.end - ev.start;
            const float er = span > 0 ? std::clamp((ratio - ev.start) / span, 0.0f, 1.0f) : 0;
            const EmitterRule& em = *ev.emitter;
            const float evp = lerp(ev.playback_min, ev.playback_max, st.rate_r);
            if (em.one_shot) {
                if (!st.fired) {
                    st.fired = true;
                    spawn(i, er, (int)std::lround(scalar(st.rate_r, em.spawn_rate, er)));
                }
            } else {
                st.accumulated += std::max(0.0f, scalar(st.rate_r, em.spawn_rate, er)) * dt * evp;
                const int n = (int)st.accumulated;
                st.accumulated -= (float)n;
                if (n > 0) spawn(i, er, n);
            }
        }
        if (time_ >= duration_) {
            loop_++;
            if (rule_->loops < 0 || loop_ < rule_->loops) {
                time_ -= duration_;
                for (auto& st : emitters_) st.fired = false;
            } else {
                timeline_done_ = true;
            }
        }
    }

    // ---- the particles
    for (size_t bi = 0; bi < batches_.size(); bi++) {
        ParticleBatch& b = batches_[bi];
        b.effect_matrix = transform_;
        const ParticleRule& pr = *b.rule;
        const EmitterRule& em = *rule_->events[bi].emitter;
        auto& ps = b.particles;
        for (size_t i = 0; i < ps.size();) {
            Particle& p = ps[i];
            const float pdt = dt * p.playback;
            p.age += pdt;
            if (p.age >= p.life) { ps[i] = ps.back(); ps.pop_back(); continue; }
            const float t = p.ratio();
            const float er = 0;   // the emitter scalars are taken at the particle's birth
            if (pr.acceleration) {
                Vec3 a = xyz(range(p, pr, pr.acceleration->min, pr.acceleration->max, t));
                if (pr.acceleration->space == 1) a = vector(transform_, a);
                a = a * (em.acceleration_scale.at(er).x / 100);
                if (pr.acceleration->zoom) a = a * zoom_;
                if (pr.acceleration->gravity) a.z -= 9.8f;
                p.velocity = p.velocity + a * pdt;
            }
            if (pr.dampening) {
                Vec3 d = xyz(range(p, pr, pr.dampening->min, pr.dampening->max, t)) * (em.damping_scale.at(er).x / 100);
                Vec3 v = p.velocity;
                if (pr.dampening->space == 1) {   // damping per axis of the effect: into its space and back
                    Vec3 ax{transform_.at(0, 0), transform_.at(0, 1), transform_.at(0, 2)}, ay{transform_.at(1, 0), transform_.at(1, 1), transform_.at(1, 2)},
                         az{transform_.at(2, 0), transform_.at(2, 1), transform_.at(2, 2)};
                    Vec3 lv{dot3(v, ax), dot3(v, ay), dot3(v, az)};
                    lv = {lv.x * std::max(0.0f, 1 - d.x * pdt), lv.y * std::max(0.0f, 1 - d.y * pdt), lv.z * std::max(0.0f, 1 - d.z * pdt)};
                    p.velocity = ax * lv.x + ay * lv.y + az * lv.z;
                } else {
                    p.velocity = {v.x * std::max(0.0f, 1 - d.x * pdt), v.y * std::max(0.0f, 1 - d.y * pdt), v.z * std::max(0.0f, 1 - d.z * pdt)};
                }
            }
            if (pr.noise) {
                const Vec3 vn = xyz(range(p, pr, pr.noise->vel_min, pr.noise->vel_max, t));
                const uint32_t frame = (uint32_t)(p.age * 30);
                Vec3 j{hash01(p.seed ^ frame, 2000) * 2 - 1, hash01(p.seed ^ frame, 2001) * 2 - 1, hash01(p.seed ^ frame, 2002) * 2 - 1};
                p.velocity = p.velocity + mulv(j, vn) * pdt;
            }
            if (pr.velocity) p.position = p.position + p.velocity * pdt;
            if (pr.rotation) {
                const Vec3 speed = xyz(range(p, pr, pr.rotation->speed_min, pr.rotation->speed_max, t));
                if (pr.rotation->accumulate) p.angles = p.angles + speed * pdt;
                else p.angles = xyz(range(p, pr, pr.rotation->init_min, pr.rotation->init_max, t));
                p.angle = p.angles.x;
            }
            if (pr.size) {
                Vec3 s = xyz(range(p, pr, pr.size->min, pr.size->max, t));
                if (pr.size->proportional) {
                    const Vec4 lo = pr.size->min.at(t), hi = pr.size->max.at(t);
                    const float k = pick(p, pr, pr.size->min);
                    s = {lerp(lo.x, hi.x, k), lerp(lo.y, hi.y, k), lerp(lo.z, hi.z, k)};
                }
                p.size = mulv(s, p.size_scale);
                p.tblr = pr.size->tblr.at(t);
                const Vec4 tv = pr.size->tblr_velocity.at(t);
                if (tv.x != 0 || tv.y != 0 || tv.z != 0 || tv.w != 0) {
                    const float speed = p.velocity.length();
                    p.tblr = {p.tblr.x + tv.x * speed, p.tblr.y + tv.y * speed, p.tblr.z + tv.z * speed, p.tblr.w + tv.w * speed};
                }
            }
            if (pr.colour) {
                const auto& c = *pr.colour;
                Vec4 col = c.min.at(t);
                if (c.max_enable) col = lerp4(col, c.max.at(t), pick(p, pr, c.min));
                if (c.can_tint) col = {col.x * p.tint.x, col.y * p.tint.y, col.z * p.tint.z, col.w * p.tint.w};
                p.colour = col;
                p.emissive = scalar(pick(p, pr, c.emissive), c.emissive, t);
            }
            if (pr.animate) {
                const auto& a = *pr.animate;
                const float first = (float)pr.frame_min;
                const float last = (float)(a.last_frame >= 0 ? (uint32_t)a.last_frame : pr.frame_max);
                const float count = std::max(1.0f, last - first + 1);
                if (a.over_life) {
                    p.frame = first + t * (count - (a.blend ? 1.0f : 0.0f));
                } else {
                    p.frame += p.frame_rate * pdt;
                    if (p.frame > last + (a.blend ? 0.0f : 0.999f)) {
                        if (a.loop == 1) p.frame = first + std::fmod(p.frame - first, count);
                        else if (a.loop == 2) {
                            const float k = std::fmod(p.frame - first, 2 * count);
                            p.frame = first + (k < count ? k : 2 * count - k);
                        } else p.frame = last;
                    }
                }
                if (!a.blend) p.frame = std::floor(p.frame);
            }
            i++;
        }
    }
}

std::vector<Light> EffectInstance::lights(float intensity_mult) const {
    // one light per emitter, from its newest particle: the game's lit emitters spawn short-lived particles fast (a
    // campfire's glow: 1800 a second living 0.01 - 0.08 s), so the light flickers with each one's random values
    std::vector<Light> out;
    for (auto& b : batches_) {
        if (!b.rule->light || b.particles.empty()) continue;
        const auto& lb = *b.rule->light;
        const Particle* newest = &b.particles[0];
        for (auto& p : b.particles) if (p.age < newest->age) newest = &p;
        const Particle& p = *newest;
        const float t = p.ratio();
        const float k = pick(p, *b.rule, lb.rgb_min);
        Light l;
        l.type = LightType::Point;
        l.position = p.position;
        l.colour = lb.colour_from_particle ? Vec3{p.colour.x, p.colour.y, p.colour.z} : xyz(lerp4(lb.rgb_min.at(t), lb.rgb_max.at(t), k));
        l.intensity = scalar(k, lb.intensity, t) * intensity_mult;
        l.range = scalar(k, lb.range, t);
        l.falloff_exponent = 8;
        l.corona_size = scalar(k, lb.corona_size, t);
        l.corona_intensity = scalar(k, lb.corona_intensity, t);
        l.corona_z_bias = lb.corona_z_bias;
        l.seed = p.seed;
        if (l.intensity > 0 && l.range > 0) out.push_back(l);
    }
    return out;
}

} // namespace rk::render
