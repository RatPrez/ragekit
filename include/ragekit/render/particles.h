// ragekit render - the game's particle effects: a .ypt's rules decoded (effect, emitter and particle rules, their domains,
// behaviours and keyframes) and played: an EffectInstance spawns and moves particles and gives what a backend draws.
//
// What the files hold is read as the game stores it (CodeWalker's Particle.cs names the fields). How the game plays it is CPU
// code the game's executable keeps encrypted, so the simulation is a reconstruction from the field names, the game's data
// and its shaders (ptfx_sprite.fxc gives the sprite's inputs); ragekit-viewer docs/particles.md lists what is known and what
// is reconstructed. Times are seconds, angles degrees, distances metres; game space is Z up.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include "ragekit/particle.h"
#include "ragekit/render/light.h"
#include "ragekit/types.h"

namespace rk {
class GameIndex;
}

namespace rk::render {

/// A keyframed value (ptxKeyframeProp): Vec4 values over a time 0..1 (the effect's, the emitter's or the particle's life).
struct Keyframe {
    uint32_t name = 0;            // the property's name hash ("ptxu_Colour:m_rgbaMinKFP")
    std::vector<float> times;
    std::vector<Vec4> values;
    uint8_t random_index = 0;     // which of a particle's random numbers picks between min and max
    bool invert_bias = false;     // uses 1 - that random number
    Vec4 fallback;                // what an empty keyframe gives (the property's default)

    bool empty() const { return values.empty(); }
    /// Linear between keyframes, held before the first and after the last.
    Vec4 at(float t) const;
};

enum class DomainShape : uint8_t { Box = 0, Sphere = 1, Cylinder = 2, Attractor = 3 };

/// Where particles start (creation), where they head (target), what pulls them (attractor).
struct Domain {
    DomainShape shape = DomainShape::Box;
    bool world_space = false, point_relative = false, creation_relative = false, target_relative = false;
    Keyframe position, rotation, size_outer, size_inner;   // over the emitter's life
};

struct EmitterRule {
    std::string name;
    std::optional<Domain> creation, target, attractor;
    Keyframe spawn_rate, spawn_rate_distance, life, playback_rate, speed, size_scale, acceleration_scale, damping_scale,
             matrix_weight_scale, inherit_velocity;   // over the emitter's life; x min, y max where a range
    bool one_shot = false;
};

enum class Alignment : int { CameraFacing = 0, Velocity = 1, WorldAxis = 2, EffectAxis = 3, VelocityPlane = 4 };

/// A shader variable of a particle rule (ptxShaderVar*): a constant, a keyframe over the particle's life, or a texture.
struct ShaderVar {
    uint32_t name = 0;            // joaat of the fxc variable's annotation name ("softness", "superalpha" ...)
    uint8_t type = 0;             // 2 vector2, 4 vector4, 6 texture, 7 keyframe
    Vec4 value;
    std::vector<std::pair<float, float>> keys;   // type 7: (time, value)
    std::string texture;

    float at(float t) const;      // a keyframe's value (or value.x)
};

/// A particle rule: how one particle moves, looks and draws.
struct ParticleRule {
    std::string name, shader, technique;   // "ptfx_sprite", "RGBA_lit_soft"
    int cull_mode = 0, blend_set = 0, lighting_mode = 0;
    bool depth_write = false, depth_test = true, alpha_blend = true;
    uint32_t frame_min = 0, frame_max = 0; // TexFrameIDMin / Max
    uint32_t diffuse_mode = 0;             // 0 RGBA, 4 RGB (alpha from the colour)
    bool lit = false, soft = false, screen_space = false, refract = false, normal_spec = false;
    uint8_t sort_type = 0, draw_type = 0;  // draw: 0 sprite, 1 model, 2 trail
    std::vector<ShaderVar> vars;
    std::string texture;                   // the diffuse (DiffuseTex2)
    std::vector<std::string> drawables;    // ptxd_Model's choices

    // behaviours (present when the rule has them)
    struct Acceleration { Keyframe min, max; int space = 0; bool zoom = false, gravity = false; };
    struct Rotation { Keyframe init_min, init_max, speed_min, speed_max; int init_mode = 0, update_mode = 0; bool accumulate = true; float speed_fade = 0; };
    struct Size { Keyframe min, max, tblr, tblr_velocity; int mode = 0; bool proportional = false; };
    struct Dampening { Keyframe min, max; int space = 0; bool air_resistance = false; };
    struct Animate { Keyframe rate; int mode = 0, last_frame = -1, loop = 0; bool random_start = false, over_life = false, hold_last = false, blend = false; };
    struct Colour { Keyframe min, max, emissive; int mode = 0; bool max_enable = false, proportional = false, can_tint = false; };
    struct Sprite { Vec3 axis; Alignment align = Alignment::CameraFacing; float flip_u = 0, flip_v = 0, near_clip = 0, far_clip = 0, projection = 0; bool screen = false, disabled = false; };
    struct Noise { Keyframe pos_min, pos_max, vel_min, vel_max; int space = 0; bool constant_speed = false; };
    struct Collision { Keyframe bounce, bounce_dir; float radius_mult = 1, rest_speed = 0; int chance = 100, kill_chance = 0; };
    struct Light { Keyframe rgb_min, rgb_max, intensity, range, corona_rgb_min, corona_rgb_max, corona_intensity, corona_size, corona_flare;
                   float corona_z_bias = 0; bool corona_use_light_colour = false, colour_from_particle = false, colour_per_frame = false,
                   intensity_per_frame = false, range_per_frame = false, shadows = false; int type = 0; };
    struct Trail { Keyframe tex_info; Vec3 axis; Alignment align = Alignment::CameraFacing; int tess_u = 1, tess_v = 1; bool wrap_over_life = false, disabled = false; };
    struct ZCull { Keyframe height, fade; int mode = 0, space = 0; };
    std::optional<Acceleration> acceleration;
    bool velocity = false;                 // ptxu_Velocity: positions follow velocities
    std::optional<Rotation> rotation;
    std::optional<Size> size;
    std::optional<Dampening> dampening;
    std::optional<Keyframe> matrix_weight;
    std::optional<Animate> animate;
    std::optional<Colour> colour;
    std::optional<Sprite> sprite;
    std::optional<Keyframe> wind;
    std::optional<Noise> noise;
    std::optional<Collision> collision;
    std::optional<Light> light;
    std::optional<Trail> trail;
    std::optional<ZCull> zcull;
    std::vector<std::string> unsimulated;  // behaviours the player does not do (decals, fog volumes, liquids ...)
    struct BiasLink { std::vector<uint32_t> props; uint8_t random_index = 0; };
    std::vector<BiasLink> bias_links;      // properties that share one random number

    const ShaderVar* var(std::string_view name) const;
};

/// One emitter of an effect's timeline: an emitter rule spawning a particle rule from start to end (ratios of the effect).
struct EventEmitter {
    float start = 0, end = 1;
    std::shared_ptr<const EmitterRule> emitter;
    std::shared_ptr<const ParticleRule> particle;
    float playback_min = 1, playback_max = 1, zoom_min = 100, zoom_max = 100;
    uint32_t tint_min = 0xFFFFFFFF, tint_max = 0xFFFFFFFF;   // ARGB bytes as stored
};

struct EffectRule {
    std::string name;
    int loops = 0;                         // -1 forever
    float duration_min = 1, duration_max = 1, playback_min = 1, playback_max = 1;
    float preupdate = 0, preupdate_interval = 0.25f;
    Vec3 random_offset;
    Keyframe tint_min, tint_max, zoom, data_sphere, data_capsule;   // over the effect's life
    bool tint_max_enable = false;
    std::vector<EventEmitter> events;
    std::vector<std::string> evolutions;   // the names scripts set ("fade", "speed" ...); not applied yet
};

/// A frame grid of a texture: frames are cells of `columns` x `rows`.
struct FrameGrid { uint32_t columns = 1, rows = 1; };

/// The rules of one or more .ypt files, with their textures and drawables.
class EffectLibrary {
public:
    /// Decodes a dictionary's rules (throws rk::Error on a rule the decoder does not understand).
    void add(std::shared_ptr<const ParticleDictionary> dict);
    /// The dictionary of that name the game uses at the high particle quality (ptfx_hi.rpf, else ptfx.rpf, else any).
    static std::shared_ptr<const ParticleDictionary> load(const GameIndex& gi, std::string_view name);

    std::shared_ptr<const EffectRule> effect(std::string_view name) const;
    std::vector<std::string> effect_names() const;
    /// A texture of any added dictionary.
    const Texture* texture(std::string_view name) const;
    /// A drawable of any added dictionary by name.
    std::shared_ptr<const Drawable> drawable(std::string_view name) const;
    /// The grid of a texture's frames. Not stored in the files (nor in its texture's header): found from the texture, the
    /// columns and rows (holding 1 + the highest frame any rule uses of it) whose cell edges fall on its empty gutters.
    FrameGrid frames(std::string_view texture) const;

    const std::vector<std::shared_ptr<const ParticleDictionary>>& dictionaries() const { return dicts_; }

private:
    std::vector<std::shared_ptr<const ParticleDictionary>> dicts_;
    std::map<std::string, std::shared_ptr<const EffectRule>, std::less<>> effects_;
    std::map<std::string, uint32_t, std::less<>> max_frame_;
    std::map<std::string, std::shared_ptr<const Drawable>, std::less<>> drawables_;
    mutable std::map<std::string, FrameGrid, std::less<>> grids_;
    mutable std::mutex grid_mutex_;
};

/// One particle as the backends draw it.
struct Particle {
    Vec3 position, velocity;
    Vec4 colour{1, 1, 1, 1};     // rgba as the rule gives it (the shader squares rgb to light it)
    float emissive = 0;
    Vec3 size{1, 1, 1};          // width, height, depth
    Vec4 tblr{1, 1, 1, 1};       // the sprite's extents from its centre: top, bottom, left, right (scalars of size / 2)
    float angle = 0;             // degrees, about the alignment's normal
    Vec3 angles;                 // degrees about x, y, z (models; x is `angle`)
    float frame = 0;             // texture frame (fraction: the blend into the next)
    bool flip_u = false, flip_v = false;
    float age = 0, life = 1;     // seconds
    float ratio() const { return life > 0 ? age / life : 1; }
    // internal
    float angular_speed = 0, frame_rate = 0, playback = 1;
    Vec3 angular_speeds;
    Vec3 size_scale{1, 1, 1};
    uint32_t seed = 0;
    float alpha_scale = 1;
    Vec4 tint{1, 1, 1, 1};
};

/// What one event emitter has alive this frame.
struct ParticleBatch {
    std::shared_ptr<const ParticleRule> rule;
    std::vector<Particle> particles;
    Mat4 effect_matrix;          // the effect's placement (EffectAxis alignment)
    FrameGrid grid;
};

/// A playing effect.
class EffectInstance {
public:
    EffectInstance(std::shared_ptr<const EffectRule> rule, const EffectLibrary& lib, uint32_t seed = 1);

    void set_transform(const Mat4& m) { transform_ = m; }
    const Mat4& transform() const { return transform_; }
    /// Advances by dt seconds (the effect's and the particles' playback rates applied).
    void update(float dt);
    /// Starts over.
    void restart();
    /// No particle alive and the timeline over (never for a looping effect).
    bool finished() const;
    float time() const { return time_; }

    const std::vector<ParticleBatch>& batches() const { return batches_; }
    const EffectLibrary& library() const { return *lib_; }
    const EffectRule& rule() const { return *rule_; }
    /// The lights its particles give (ptxu_Light), world space. `intensity_mult`: the timecycle's particle light mult.
    std::vector<Light> lights(float intensity_mult = 1) const;
    size_t particle_count() const;

private:
    struct EmitterState { float accumulated = 0; bool fired = false; float rate_r = 0; };
    void spawn(size_t event, float emitter_ratio, int count);
    float rand();

    std::shared_ptr<const EffectRule> rule_;
    const EffectLibrary* lib_;
    Mat4 transform_ = Mat4::identity();
    std::vector<ParticleBatch> batches_;
    std::vector<EmitterState> emitters_;
    float time_ = 0, duration_ = 1, playback_ = 1, zoom_ = 1;
    int loop_ = 0;
    bool timeline_done_ = false;
    std::mt19937 rng_;
    uint32_t next_seed_ = 1;
};

} // namespace rk::render
