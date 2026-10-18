#include "ragekit/render/material.h"
#include <algorithm>
#include <cmath>
#include <string_view>
#include "ragekit/hash.h"

namespace rk::render {

namespace {

bool starts(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool has(std::string_view s, std::string_view w) { return s.find(w) != std::string_view::npos; }

float scalar(const Shader& s, std::string_view name, float def) {
    auto* p = s.find(name);
    return p && p->type != ParamType::Texture && !p->values.empty() ? p->values[0].x : def;
}
Vec4 vec(const Shader& s, std::string_view name, Vec4 def) {
    auto* p = s.find(name);
    return p && p->type != ParamType::Texture && !p->values.empty() ? p->values[0] : def;
}

/// What the per-family rules fill in; make_material turns it into the Material.
struct Rules {
    SpecIntensity spec = SpecIntensity::None;
    SpecGloss gloss = SpecGloss::Param;
    FresnelSource fresnel = FresnelSource::Param;
    Sheen sheen = Sheen::None;
    Emissive emissive = Emissive::None;
    AlphaMode alpha = AlphaMode::Standard;
    float mult, falloff, fres, bump;   // spec.x .. spec.w
    float sheen_intensity = 0, sheen_factor = 0;
    float diffuse_tile = 1, spec_tile = 1;
    int livery_uv = 1, spec_uv = 0, dirt_uv = -1;
    uint32_t features = 0;
};

/// Props, map buildings, glass: the "megashader" family (research 3.2).
void megashader(std::string_view n, Rules& r) {
    auto& f = r.features;
    if (has(n, "_tnt")) f |= FeatTint;
    if (has(n, "detail")) f |= FeatDetail;
    if (has(n, "reflect")) f |= FeatReflect;
    auto map = [&] { r.spec = SpecIntensity::MapMask; r.gloss = SpecGloss::MapA; };
    auto param = [&] { r.spec = SpecIntensity::Param; r.gloss = SpecGloss::Param; };
    if (n == "cpv_only") { f |= FeatWhite; return; }
    if (starts(n, "cutout_fence")) { if (has(n, "normal")) f |= FeatNormalMap; return; }   // vertex normal, no spec
    if (starts(n, "default") || starts(n, "cutout") || n == "albedo_alpha" || n == "emissive_additive_alpha") {
        if (starts(n, "default_spec")) param();
        return;
    }
    if (starts(n, "emissive")) {
        r.emissive = Emissive::Mask;
        if (n == "emissive_speclum") { r.spec = SpecIntensity::Luminance; r.gloss = SpecGloss::Luminance; }
        return;
    }
    if (starts(n, "decal")) {
        if (n == "decal_normal_only") { f |= FeatNormalMap; return; }
        f |= FeatKeepNormal;
        param();
        return;
    }
    if (starts(n, "spec")) {
        if (n == "spec_decal") f |= FeatKeepNormal;
        map();
        return;
    }
    if (starts(n, "normal") && !has(n, "spec")) { f |= FeatNormalMap; param(); return; }
    // normal_spec (+detail, tnt, reflect, decal, emissive, pxm), glass*, trees*: the base case
    if (!(starts(n, "glass") && !has(n, "env"))) f |= FeatNormalMap;
    map();
    if (has(n, "emissive")) r.emissive = Emissive::Mask;
}

/// Vehicles (research 2): every one paints, multiplies albedo and spec by COLOR0.r and has dirt on uv1 unless noted.
void vehicle(std::string_view n, const Shader& s, Rules& r) {
    auto& f = r.features;
    f |= FeatPaint | FeatAlbedoR | FeatSpecR | FeatDirt;
    r.dirt_uv = 1;
    auto fixed = [&](SpecIntensity si, float mult, SpecGloss g, float falloff, float fres) {
        r.spec = si; r.mult = mult; r.gloss = g; r.falloff = falloff; r.fres = fres;
    };
    if (starts(n, "vehicle_paint")) {
        const int k = n.size() > 13 && n[13] >= '0' && n[13] <= '9' ? n[13] - '0' : 1;
        r.spec = k == 1 ? SpecIntensity::Param : SpecIntensity::MapMask;   // paint1: the map is not read
        r.gloss = k == 1 ? SpecGloss::Param : SpecGloss::MapA;
        r.sheen = Sheen::Paint;
        if (k >= 2) r.dirt_uv = 0;
        if (k == 3 || k == 7) r.spec_uv = 1;
        if (k >= 3) f |= FeatLivery;
        if (k >= 4 && k <= 6) {
            r.diffuse_tile = scalar(s, "DiffuseTexTileUV", 8);
            r.spec_tile = scalar(s, "specTexTileUV", 1);
            r.sheen_intensity = scalar(s, "specular2ColorIntensity", 0);
            r.sheen_factor = scalar(s, "specular2Factor", 40);
            r.sheen = r.sheen_intensity > 0 ? Sheen::Paint4 : Sheen::None;
        }
        if (k == 7) r.diffuse_tile = scalar(s, "DiffuseTexTileUV", 1);
        if (k >= 8) { r.spec_uv = 1; f |= FeatNormalMap; r.dirt_uv = -1; }
    } else if (starts(n, "vehicle_tire")) {
        f |= FeatNormalMap;
        fixed(SpecIntensity::MapR2, 1, SpecGloss::MapG2, 512, r.fres);
        r.fresnel = FresnelSource::OneMinusSpecB;
        r.sheen = Sheen::Tyre;
    } else if (starts(n, "vehicle_shuts")) {
        f |= FeatNormalMap;
        r.bump = 0.6f;
        fixed(SpecIntensity::MapMask, 0.065f, SpecGloss::MapA, 179, 0.83f);
        r.dirt_uv = -1;
    } else if (starts(n, "vehicle_interior")) {
        r.spec = SpecIntensity::None;
        r.dirt_uv = -1;
    } else if (starts(n, "vehicle_detail2")) {
        fixed(SpecIntensity::MapMask, 1, SpecGloss::MapA, 15, 0.8f);
        r.dirt_uv = -1;
    } else if (starts(n, "vehicle_emissive_opaque")) {
        fixed(SpecIntensity::MapMask, 0.8f, SpecGloss::MapA, 510, 0.96f);
        r.emissive = Emissive::VehicleOpaque;
    } else if (starts(n, "vehicle_dash_emissive_opaque")) {
        fixed(SpecIntensity::MapMask, 0.8f, SpecGloss::MapA, 510, 0.96f);
        r.emissive = Emissive::DashOpaque;
    } else if (starts(n, "vehicle_dash_emissive")) {
        r.spec = SpecIntensity::None;
        r.emissive = Emissive::DashLights;
        f &= ~FeatAlbedoR;
        r.alpha = AlphaMode::LightId;
    } else if (starts(n, "vehicle_vehglass")) {
        const bool inner = has(n, "inner");
        fixed(SpecIntensity::MapMask, inner ? 0.05f : 1, SpecGloss::MapA, inner ? 512 : 510, 0.96f);
        r.alpha = AlphaMode::VehicleGlass;
        f |= FeatTwoSided;
    } else if (starts(n, "vehicle_lights")) {   // lights, lightsemissive, lightsemissive_siren
        fixed(SpecIntensity::MapMask, 0.8f, SpecGloss::MapA, 510, 0.96f);
        f &= ~(FeatSpecR | FeatAlbedoR);
        r.emissive = Emissive::VehicleLights;
        r.alpha = AlphaMode::LightId;
    } else if (starts(n, "vehicle_badges")) {
        f |= FeatNormalMap;
        r.spec = SpecIntensity::MapMask; r.gloss = SpecGloss::MapA;   // as prop normal_spec
    } else if (starts(n, "vehicle_decal")) {
        fixed(SpecIntensity::MapMask, 0.25f, SpecGloss::MapA, 15, 0.96f);
    } else if (starts(n, "vehicle_blurredrotor")) {
        fixed(SpecIntensity::MapMask, 0.4f, SpecGloss::MapA, 512, r.fres);
    } else if (starts(n, "vehicle_licenseplate")) {
        f |= FeatNormalMap;
        fixed(SpecIntensity::Param, 0.2f, SpecGloss::Param, 500, 0.98f);
    } else {
        // vehicle_mesh (+2, _enveff), basic, generic, cutout, detail, track, cloth: as vehicle_mesh
        f |= FeatNormalMap;
        fixed(SpecIntensity::MapR2, 0.6f, SpecGloss::MapG2, 460, r.fres);
        r.fresnel = FresnelSource::OneMinusSpecB;
        r.sheen = Sheen::Mesh;
    }
}

/// Weapons (research 3.3): gloss in the spec map's green, as peds; a palette or a tint; the sheen.
void weapon(std::string_view n, const Shader& s, Rules& r) {
    auto& f = r.features;
    if (has(n, "palette")) f |= FeatPalette;
    if (has(n, "_tnt")) f |= FeatTint;
    if (has(n, "detail")) f |= FeatDetail;
    f |= FeatNormalMap;
    r.spec = SpecIntensity::MapR2;
    r.gloss = SpecGloss::MapG2;
    r.sheen = Sheen::Weapon;
    r.sheen_intensity = scalar(s, "specular2ColorIntensity", 0);
    r.sheen_factor = scalar(s, "specular2Factor", 0);
    if (has(n, "emissivestrong")) { r.spec = SpecIntensity::None; r.sheen = Sheen::None; r.emissive = Emissive::AlphaMult; }
    else if (has(n, "emissive")) r.emissive = Emissive::Mask;
    if (has(n, "_alpha") && !has(n, "emissive")) { r.spec = SpecIntensity::MapMask; r.gloss = SpecGloss::MapA; r.sheen = Sheen::None; }
    if (has(n, "cutout")) r.sheen = Sheen::None;
}

/// Peds (rat-tooling research/ped-shaders.md): spec intensity in the map's red², gloss in its green², AO squared, the
/// diffuse alone as alpha (ped_default and ped_default_palette also take COLOR0.a).
Family ped(std::string_view n, Rules& r) {
    auto& f = r.features;
    f |= FeatAoSquared;
    r.alpha = AlphaMode::Diffuse;
    if (has(n, "palette")) f |= FeatPalette;
    auto full = [&] {
        f |= FeatNormalMap | FeatSkin;
        r.spec = SpecIntensity::MapR2;
        r.gloss = SpecGloss::MapG2;
    };
    if (starts(n, "ped_hair")) {                    // ped_hair_spiked, ped_hair_cutout_alpha (+_mask, _enveff, _cloth)
        full();
        f &= ~FeatSkin;
        f |= FeatHairTint;
        return Family::Hair;
    }
    if (starts(n, "ped_default")) {                 // vertex normal, no spec
        if (n == "ped_default" || n == "ped_default_palette") r.alpha = AlphaMode::Standard;
        return Family::Ped;
    }
    if (n == "ped_decal_decoration") { full(); return Family::Ped; }
    if (starts(n, "ped_decal")) {                   // diffuse only, blended over the surface's normal and spec
        f |= FeatKeepNormal;
        return Family::Ped;
    }
    full();                                         // ped, ped_cloth, ped_wrinkle, ped_alpha, ped_emissive, ...
    if (starts(n, "ped_emissive")) r.emissive = Emissive::PedMask;
    return Family::Ped;
}

/// Terrain (terrain_cb_*.fxc, the deferred PS): four diffuse / bump layers blended by w = (r, g, b) of COLOR1 or of
/// the lookup texture: layer0 (1-g)(1-b), layer1 (1-g)b, layer2 g(1-b), layer3 gb, all times (1-r); spec intensity
/// specularIntensityMult², gloss specularFalloffMult, fresnel 0.98.
void terrain(std::string_view n, const Shader& s, Rules& r) {
    auto& f = r.features;
    if (!has(n, "_lod")) f |= FeatNormalMap;
    if (has(n, "_cm") || has(n, "2tex_blend")) f |= FeatLookup;
    if (has(n, "_tnt")) f |= FeatTint;
    const float i = scalar(s, "specularIntensityMult", 0);
    r.spec = SpecIntensity::Param;
    r.gloss = SpecGloss::Param;
    r.mult = i * i;
    r.falloff = scalar(s, "specularFalloffMult", 48);
    r.fres = 0.98f;
}

// The game's 64 hair colours (the character creator's numbering), as the game reports them (rat-tooling
// shared/ui/gta/shading.js GAME_HAIR, dumped from a server).
const uint32_t kHairColours[64] = {
    0x1c1f21, 0x272a2c, 0x312e2c, 0x35261c, 0x4b321f, 0x5c3b24, 0x6d4c35, 0x6b503b, 0x765c45, 0x7f684e, 0x99815d,
    0xa79369, 0xaf9c70, 0xbba063, 0xd6b97b, 0xdac38e, 0x9f7f59, 0x845039, 0x682b1f, 0x61120c, 0x640f0a, 0x7c140f,
    0xa02e19, 0xb64b28, 0xa2502f, 0xaa4e2b, 0x626262, 0x808080, 0xaaaaaa, 0xc5c5c5, 0x463955, 0x5a3f6b, 0x763c76,
    0xed74e3, 0xeb4b93, 0xf299bc, 0x04959e, 0x025f86, 0x023974, 0x3fa16a, 0x217c61, 0x185c55, 0xb6c034, 0x70a90b,
    0x439d13, 0xdcb857, 0xe5b103, 0xe69102, 0xf28831, 0xfb8057, 0xe28b58, 0xd1593c, 0xce3120, 0xad0903, 0x880302,
    0x1f1814, 0x291f19, 0x2e221b, 0x37291e, 0x2e2218, 0x231b15, 0x020202, 0x706c66, 0x9d7a50};

} // namespace

void DrawParams::set_hair(int colour, int highlight) {
    auto row = [](int i, Vec3& dark, Vec3& light) {
        const uint32_t c = kHairColours[std::clamp(i, 0, 63)];
        const float v[3] = {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f};
        dark = {v[0] * 0.3f, v[1] * 0.3f, v[2] * 0.3f};
        light = {std::min(1.f, v[0] * 1.35f + 0.04f), std::min(1.f, v[1] * 1.35f + 0.04f), std::min(1.f, v[2] * 1.35f + 0.04f)};
    };
    hair_tint = true;
    row(colour, hair[0], hair[1]);
    row(highlight >= 0 ? highlight : colour, hair[2], hair[3]);
}

const char* texture_role_name(TextureRole r) {
    switch (r) {
        case TextureRole::Diffuse: return "diffuse";
        case TextureRole::Normal: return "normal";
        case TextureRole::Specular: return "specular";
        case TextureRole::Detail: return "detail";
        case TextureRole::TintPalette: return "tint palette";
        case TextureRole::Environment: return "environment";
        case TextureRole::Diffuse2: return "livery";
        case TextureRole::Dirt: return "dirt";
        case TextureRole::Palette: return "palette";
        case TextureRole::Layer1: return "layer 1";
        case TextureRole::Layer2: return "layer 2";
        case TextureRole::Layer3: return "layer 3";
        case TextureRole::Bump1: return "bump 1";
        case TextureRole::Bump2: return "bump 2";
        case TextureRole::Bump3: return "bump 3";
        case TextureRole::Lookup: return "lookup";
        default: return "?";
    }
}

std::string shader_name(uint32_t hash) {
    auto n = known_shader_name(hash);
    return n ? n : "";
}

Material make_material(const Shader& s) {
    Material m;
    m.shader_hash = s.name_hash;
    m.shader = shader_name(s.name_hash);
    if (m.shader.empty()) m.shader = hash_name(s.name_hash);
    m.bucket = (Bucket)(s.render_bucket & 7);
    const std::string_view n = m.shader;

    Rules r;
    r.mult = scalar(s, "specularIntensityMult", 0.125f);
    r.falloff = scalar(s, "specularFalloffMult", 100);
    r.fres = scalar(s, "specularFresnel", 0.97f);
    r.bump = scalar(s, "bumpiness", 1);
    if (starts(n, "vehicle_")) { m.family = Family::Vehicle; vehicle(n, s, r); }
    else if (starts(n, "weapon_")) { m.family = Family::Weapon; weapon(n, s, r); }
    else if (starts(n, "ped")) m.family = ped(n, r);
    else if (starts(n, "terrain")) { m.family = Family::Terrain; terrain(n, s, r); }
    else {
        if (starts(n, "water") || starts(n, "grass")) m.family = Family::Unsupported;
        megashader(n, r);
        if (r.spec == SpecIntensity::None) r.fres = 0.98f;   // no spec: the effect's constant
    }
    if (n == "cpv_only" || n == "trees_shadow_proxy" || n == "decal_shadow_only") r.features |= FeatShadowOnly;
    // decal_dirt (rat-tooling research, drawable shaders 1.3 / 3.2): albedo diffuse * COLOR0.rgb, alpha dot(diffuse.rgb, dirtDecalMask) * COLOR0.a
    if (n == "decal_dirt") r.alpha = AlphaMode::DirtDecal;
    // decal_normal_only writes the G-buffer's normal alone (no albedo, no spec): a forward backend cannot bend the surface under it, so it is
    // left out of the colour pass
    if (n == "decal_normal_only") r.features |= FeatShadowOnly;
    if (r.spec == SpecIntensity::None || (m.family == Family::Terrain && r.mult <= 0)) { r.mult = 0; r.spec = SpecIntensity::None; }

    m.features = r.features;
    m.spec = r.spec; m.gloss = r.gloss; m.fresnel = r.fresnel;
    m.sheen = r.sheen; m.emissive = r.emissive; m.alpha = r.alpha;

    auto& c = m.constants;
    c.spec = {r.mult, r.falloff, r.fres, std::max(r.bump, 0.001f)};
    Vec4 mask = vec(s, "specMapIntMask", {1, 0, 0, 0});
    c.spec_mask = {mask.x, mask.y, mask.z, scalar(s, "emissiveMultiplier", 1)};
    c.detail = vec(s, "detailSettings", {0, 0, 1, 1});
    if (s.find("globalAnimUV0") && s.find("globalAnimUV1")) {
        m.features |= FeatUvAnim;
        c.uv_anim0 = vec(s, "globalAnimUV0", {1, 0, 0, 0});
        c.uv_anim1 = vec(s, "globalAnimUV1", {0, 1, 0, 0});
    }
    c.misc = {scalar(s, "reflectivePower", 0), m.state.alpha_ref, r.sheen_intensity, r.sheen_factor};
    c.tiling = {r.diffuse_tile, r.spec_tile, 0, 0};
    if (m.has(FeatPaint)) {
        // matDiffuseColor: (2, n, n) is paint slot n the engine fills; anything else is a literal colour
        Vec4 md = vec(s, "matDiffuseColor", {1, 1, 1, 1});
        if (std::lround(md.x) == 2 && std::lround(md.y) == std::lround(md.z) && md.y >= 1 && md.y <= 7) c.colour = {1, 1, 1, (float)std::lround(md.y)};
        else c.colour = {md.x, md.y, md.z, 0};
    }
    Vec4 dc = vec(s, "dirtColor", {0.27f, 0.25f, 0.22f, 0});
    c.dirt_colour = {dc.x, dc.y, dc.z, 0};
    if (m.alpha == AlphaMode::DirtDecal) {   // decal_dirt has no dirt: the slot carries dirtDecalMask (default R)
        Vec4 mask = vec(s, "dirtDecalMask", {1, 0, 0, 0});
        c.dirt_colour = {mask.x, mask.y, mask.z, 0};
    }
    c.uv_sets[0] = r.livery_uv; c.uv_sets[1] = r.spec_uv; c.uv_sets[2] = r.dirt_uv;
    if (m.family == Family::Hair) {
        Vec4 ai = vec(s, "anisotropicSpecularIntensity", {0.1f, 0.15f, 0, 0});
        Vec4 ae = vec(s, "anisotropicSpecularExponent", {16, 32, 0, 0});
        Vec4 ac = vec(s, "anisotropicSpecularColour", {0.1f, 0.1f, 0.1f, 1});
        c.hair = {ai.x, ai.y, ae.x, ae.y};
        c.hair_colour = {ac.x, ac.y, ac.z, 0};
    }

    // textures by role
    auto bind = [&](TextureRole role, std::string_view sampler) {
        auto& t = m.textures[(size_t)role];
        if (t.used()) return;
        auto name = s.texture_of(sampler);
        if (name.empty()) return;
        t.hash = joaat_lower(name);
        t.name = std::move(name);
    };
    bind(TextureRole::Diffuse, "DiffuseSampler");
    if (m.family == Family::Terrain) {
        bind(TextureRole::Diffuse, "TextureSampler_layer0");
        bind(TextureRole::Layer1, "TextureSampler_layer1");
        bind(TextureRole::Layer2, "TextureSampler_layer2");
        bind(TextureRole::Layer3, "TextureSampler_layer3");
        if (m.has(FeatNormalMap)) {
            bind(TextureRole::Normal, "BumpSampler_layer0");
            bind(TextureRole::Bump1, "BumpSampler_layer1");
            bind(TextureRole::Bump2, "BumpSampler_layer2");
            bind(TextureRole::Bump3, "BumpSampler_layer3");
        }
        if (m.has(FeatLookup)) bind(TextureRole::Lookup, "lookupSampler");
    }
    bind(TextureRole::Diffuse, "PlateBgSampler");
    if (m.has(FeatNormalMap)) { bind(TextureRole::Normal, "BumpSampler"); bind(TextureRole::Normal, "PlateBgBumpSampler"); }
    if (m.spec == SpecIntensity::MapR2 || m.spec == SpecIntensity::MapMask || m.gloss == SpecGloss::MapA
        || m.gloss == SpecGloss::MapG2 || m.fresnel == FresnelSource::OneMinusSpecB || m.sheen != Sheen::None)
        bind(TextureRole::Specular, "SpecSampler");
    if (m.has(FeatDetail)) bind(TextureRole::Detail, "DetailSampler");
    if (m.has(FeatTint)) bind(TextureRole::TintPalette, "TintPaletteSampler");
    if (m.has(FeatReflect)) bind(TextureRole::Environment, "EnvironmentSampler");
    if (m.has(FeatLivery)) bind(TextureRole::Diffuse2, "DiffuseSampler2");
    if (m.has(FeatDirt)) bind(TextureRole::Dirt, "DirtSampler");
    if (m.has(FeatPalette))
        for (auto& p : s.params)   // weapon palettes: the sampler with "pal" in its name (not the tint palette)
            if (p.type == ParamType::Texture && !p.texture.empty()) {
                const char* known = known_param_name(p.name_hash);
                std::string pn = known ? known : "";
                for (auto& ch : pn) ch = (char)std::tolower((unsigned char)ch);
                if (has(pn, "pal") && !has(pn, "tint")) {
                    m.textures[(size_t)TextureRole::Palette] = {p.texture, joaat_lower(p.texture)};
                    break;
                }
            }
    auto drop = [&](Feature f, TextureRole role) { if (m.has(f) && !m.texture(role).used()) m.features &= ~f; };
    if (m.family != Family::Terrain) drop(FeatNormalMap, TextureRole::Normal);   // terrain: any layer's bump
    drop(FeatLookup, TextureRole::Lookup);
    drop(FeatTint, TextureRole::TintPalette);
    drop(FeatDetail, TextureRole::Detail);
    drop(FeatReflect, TextureRole::Environment);
    drop(FeatLivery, TextureRole::Diffuse2);
    drop(FeatDirt, TextureRole::Dirt);
    drop(FeatPalette, TextureRole::Palette);

    // pipeline state by bucket (research 1.3, 2.5): opaque and cut-out are deferred, decals deferred and alpha-blended,
    // the alpha bucket forward and blended
    auto& st = m.state;
    switch (m.bucket) {
        case Bucket::Cutout:
            st.alpha_test = true;
            st.alpha_to_coverage = starts(n, "trees") || m.family == Family::Hair;   // the game resolves hair fringes
            m.features |= FeatDeferred;
            break;
        case Bucket::Decal:
            st.blend = Blend::Alpha; st.depth_write = false; st.polygon_offset = true;
            if (m.family == Family::Megashader) m.features |= FeatDeferred;
            break;
        case Bucket::Alpha:
            st.blend = n == "emissive_additive_alpha" ? Blend::Additive : Blend::Alpha;
            st.depth_write = false;
            break;
        default:
            m.features |= FeatDeferred;
            break;
    }
    if (m.has(FeatTwoSided)) st.cull_back = false;
    c.modes[0] = (int32_t)m.features;
    c.modes[1] = (int32_t)m.spec;
    c.modes[2] = (int32_t)m.gloss;
    c.modes[3] = (int32_t)m.fresnel;
    c.modes2[0] = (int32_t)m.sheen;
    c.modes2[1] = (int32_t)m.emissive;
    c.modes2[2] = (int32_t)m.alpha;
    c.modes2[3] = (int32_t)m.bucket;
    c.uv_sets[3] = (int32_t)m.family;
    return m;
}

} // namespace rk::render
