#include "ragekit/render/timecycle.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include "ragekit/error.h"
#include "ragekit/gameindex.h"
#include "ragekit/hash.h"
#include "ragekit/xml_dom.h"

namespace rk::render {

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}
std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = (char)std::toupper((unsigned char)c);
    return r;
}

/// The copy of a data file the game uses, preferring one whose path holds `hint` (several files share a stem).
std::string read_text(const GameIndex& gi, const std::string& stem, const std::string& ext, std::string_view hint = {}) {
    auto versions = gi.versions(joaat_lower(stem), ext);
    if (versions.empty()) return {};
    const IndexedFile* pick = &versions.back();
    if (!hint.empty())
        for (auto it = versions.rbegin(); it != versions.rend(); ++it)
            if (lower(gi.path(*it)).find(hint) != std::string::npos) { pick = &*it; break; }
    Bytes b = gi.extract(*pick);
    return std::string(b.begin(), b.end());
}

} // namespace

Timecycle Timecycle::load(const GameIndex& gi) {
    Timecycle tc;
    const std::string time = read_text(gi, "time", ".xml", "levels/gta5/time.xml");
    if (time.empty()) throw Error("timecycle: no time.xml");
    XmlElement t = parse_xml(time);
    if (auto s = t.child("suninfo")) tc.sun_roll_ = std::strtof(s->attribute("sun_roll", "122").c_str(), nullptr);
    if (auto m = t.child("mooninfo")) tc.moon_roll_ = std::strtof(m->attribute("moon_roll", "-122").c_str(), nullptr);
    for (auto* s : t.children_named("sample")) {
        tc.hours_.push_back(std::strtof(s->attribute("hour", "0").c_str(), nullptr));
        tc.durations_.push_back(std::strtof(s->attribute("duration", "0").c_str(), nullptr));
    }
    if (tc.hours_.empty()) throw Error("timecycle: time.xml has no samples");

    const std::string weather = read_text(gi, "weather", ".xml", "levels/gta5/weather.xml");
    if (weather.empty()) throw Error("timecycle: no weather.xml");
    XmlElement w = parse_xml(weather);
    std::vector<std::pair<std::string, std::string>> files;   // weather name, file stem
    if (auto types = w.child("WeatherTypes"))
        for (auto* item : types->children_named("Item")) {
            std::string name = upper(item->child_text("Name")), file = item->child_text("TimeCycleFilename");
            if (name.empty() || file.empty()) continue;
            auto slash = file.find_last_of("/\\");
            std::string stem = lower(file.substr(slash == std::string::npos ? 0 : slash + 1));
            if (stem.size() > 4 && stem.ends_with(".xml")) stem.resize(stem.size() - 4);
            files.emplace_back(name, stem);
        }
    for (auto& [name, stem] : files) {
        const std::string text = read_text(gi, stem, ".xml", "timecycle/");
        if (text.empty()) continue;
        XmlElement doc = parse_xml(text);
        Weather wd;
        for (auto* cycle : doc.children_named("cycle"))
            for (auto* region : cycle->children_named("region")) {
                Region& r = wd.regions[upper(region->attribute("name", "GLOBAL"))];
                for (auto& v : region->children) {
                    std::vector<float> vals;
                    std::istringstream in(v.text);
                    float x;
                    while (in >> x) vals.push_back(x);
                    if (!vals.empty()) r.values[v.name] = std::move(vals);
                }
            }
        if (wd.regions.empty()) continue;
        tc.weathers_[name] = std::move(wd);
        tc.order_.push_back(name);
    }

    // the modifiers: every timecycle_mods_*.xml, later ranks replacing earlier definitions
    for (auto& f : gi.files()) {
        if (gi.extension(f) != ".xml") continue;
        const std::string name = gi.file_name(f);
        if (name.rfind("timecycle_mods_", 0) != 0) continue;
        XmlElement doc;
        try {
            Bytes b = gi.extract(f);
            doc = parse_xml(std::string_view((const char*)b.data(), b.size()));
        } catch (const std::exception&) { continue; }
        for (auto* m : doc.children_named("modifier")) {
            Modifier mod;
            for (auto& v : m->children) {
                std::istringstream vin(v.text);
                float a = 0, b = 0;
                if (vin >> a) { vin >> b; mod.values[v.name] = {a, b}; }
            }
            tc.modifiers_[lower(m->attribute("name"))] = std::move(mod);
        }
    }

    // visualsettings.dat: "key value" lines, '#' comments
    const std::string vs = read_text(gi, "visualsettings", ".dat");
    std::istringstream in(vs);
    for (std::string line; std::getline(in, line);) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string key;
        float v;
        if (ls >> key && ls >> v && key.find("//") != 0) tc.settings_[key] = v;
    }
    return tc;
}

float Timecycle::setting(std::string_view key, float fallback) const {
    auto it = settings_.find(key);
    return it == settings_.end() ? fallback : it->second;
}

float Timecycle::value(std::string_view weather, std::string_view variable, float hour, std::string_view region, float fallback) const {
    auto w = weathers_.find(upper(weather));
    if (w == weathers_.end()) return fallback;
    auto r = w->second.regions.find(upper(region));
    if (r == w->second.regions.end()) r = w->second.regions.find("GLOBAL");
    if (r == w->second.regions.end()) r = w->second.regions.begin();
    auto v = r->second.values.find(variable);
    if (v == r->second.values.end() || v->second.empty()) return fallback;
    const auto& vals = v->second;
    // time.xml: a sample holds its value for `duration` hours, then blends linearly into the next (wrapping past 24)
    float h = std::fmod(hour, 24.0f);
    if (h < 0) h += 24;
    const size_t n = std::min(hours_.size(), vals.size());
    for (size_t i = 0; i < n; i++) {
        const bool last = i + 1 >= n;
        const float next = last ? hours_[0] + 24 : hours_[i + 1];
        const float hh = (last && h < hours_[0]) ? h + 24 : h;
        if ((hh >= hours_[i] && hh < next) || last) {
            const float start = hours_[i] + durations_[i], range = next - start;
            const float blend = range > 0 ? std::clamp((hh - start) / range, 0.0f, 1.0f) : 0.0f;
            return vals[i] * (1 - blend) + vals[last ? 0 : i + 1] * blend;
        }
    }
    return vals[0];
}

std::vector<std::string> Timecycle::modifiers() const {
    std::vector<std::string> out;
    for (auto& [n, m] : modifiers_) out.push_back(n);
    return out;
}

bool Timecycle::has_modifier(std::string_view name) const { return modifiers_.count(lower(name)) != 0; }

Environment Timecycle::environment(std::string_view weather_in, float hour, std::string_view region, std::string_view modifier, float strength) const {
    std::string weather = upper(weather_in);
    if (!weathers_.count(weather)) weather = weathers_.count("EXTRASUNNY") ? "EXTRASUNNY" : (order_.empty() ? "" : order_[0]);
    const Modifier* mod = nullptr;
    if (!modifier.empty()) {
        auto it = modifiers_.find(lower(modifier));
        if (it != modifiers_.end()) mod = &it->second;
    }
    auto v = [&](std::string_view name, float fallback = 0) {
        float x = value(weather, name, hour, region, fallback);
        if (mod) {
            auto m = mod->values.find(name);
            if (m != mod->values.end()) x += (m->second.first - x) * std::clamp(strength, 0.0f, 1.0f);
        }
        return x;
    };
    auto rgb = [&](const std::string& base, Vec3 fallback = {}) {
        return Vec3{v(base + "_r", fallback.x), v(base + "_g", fallback.y), v(base + "_b", fallback.z)};
    };

    Environment e;
    e.hour = hour;
    e.sun_direction = Environment::sun_at(hour, sun_roll_, moon_roll_);
    e.sun_colour = rgb("light_dir_col") * v("light_dir_mult", 1);
    const float natural = v("natural_ambient_multiplier", 1);
    e.natural_down = rgb("light_natural_amb_down_col") * (v("light_natural_amb_down_intensity") * natural);
    e.natural_up = rgb("light_natural_amb_up_col") * (v("light_natural_amb_up_intensity") * v("light_natural_amb_up_intensity_mult", 1) * natural);
    e.artificial_ext_down = rgb("light_artificial_ext_down_col") * v("light_artificial_ext_down_intensity");
    e.artificial_ext_up = rgb("light_artificial_ext_up_col") * v("light_artificial_ext_up_intensity");
    const float interior = v("artificial_int_ambient_multiplier", 1);
    e.artificial_int_down = rgb("light_artificial_int_down_col") * (v("light_artificial_int_down_intensity") * interior);
    e.artificial_int_up = rgb("light_artificial_int_up_col") * (v("light_artificial_int_up_intensity") * interior);
    e.ambient_wrap = v("light_amb_down_wrap", 0.3f);
    e.directional_ambient = rgb("light_directional_amb_col") * (v("light_directional_amb_intensity") * v("light_directional_amb_intensity_mult", 1));
    const float sky = v("sky_hdr", 1);
    e.sky_zenith = rgb("sky_zenith_col") * (v("sky_zenith_col_inten", 1) * sky);
    e.sky_horizon = rgb("sky_zenith_transition_col") * (v("sky_zenith_transition_col_inten", 1) * sky);

    e.light_falloff_mult = v("light_falloff_mult", 1);
    e.particle_light_mult = v("particle_light_intensity_mult", 1);
    e.particle_emissive_mult = v("particle_emissive_intensity_mult", 1);
    e.corona_size = setting("misc.coronas.sizeScaleGlobal", 0.75f) * v("sprite_size", 1);
    e.corona_intensity = setting("misc.coronas.intensityScaleGlobal", 0.5f) * v("sprite_brightness", 1);

    e.exposure = v("postfx_exposure");
    e.exposure_min = v("postfx_exposure_min", -3.5f);
    e.exposure_max = v("postfx_exposure_max", 5);
    e.exposure_curve[0] = setting("Exposure.curve.scale", -106.2f);
    e.exposure_curve[1] = setting("Exposure.curve.power", 0.007f);
    e.exposure_curve[2] = setting("Exposure.curve.offset", 104.7f);
    // the tone map: visualsettings.dat's unless the weather overrides it
    auto curve = [&](const char* which, const char* suffix, float& exposure_out) {
        Tonemap t;
        const std::string vs = std::string("Tonemapping.") + which + ".filmic.";
        t.a = setting(vs + "A", t.a); t.b = setting(vs + "B", t.b); t.c = setting(vs + "C", t.c); t.d = setting(vs + "D", t.d);
        t.e = setting(vs + "E", t.e); t.f = setting(vs + "F", t.f); t.w = setting(vs + "W", t.w);
        exposure_out = setting(vs + "exposure", exposure_out);
        if (v(std::string("postfx_tonemap_filmic_override_") + which) > 0) {
            const std::string p = "postfx_tonemap_filmic_";
            t.a = v(p + "a" + suffix, t.a); t.b = v(p + "b" + suffix, t.b); t.c = v(p + "c" + suffix, t.c); t.d = v(p + "d" + suffix, t.d);
            t.e = v(p + "e" + suffix, t.e); t.f = v(p + "f" + suffix, t.f); t.w = v(p + "w" + suffix, t.w);
            exposure_out = v(p + "exposure_" + which, exposure_out);
        }
        return t;
    };
    e.tonemap_dark = curve("dark", "", e.tonemap_exposure_dark);
    e.tonemap_bright = curve("bright", "_bright", e.tonemap_exposure_bright);
    e.correct_colour = rgb("postfx_correct_col");
    e.correct_cutoff = v("postfx_correct_cutoff");
    e.shift_colour = rgb("postfx_shift_col");
    e.shift_cutoff = v("postfx_shift_cutoff");
    e.saturation = v("postfx_desaturation", 1);   // PS_Composite: lum + Desaturate * (c - lum); 1 in every weather
    e.ssao_strength = v("ssao_inten", 4);
    e.ssao_cp = v("ssao_cp_strength", 0.5f);
    e.ssao_qs = v("ssao_qs_strength", 1);
    e.reflection_interior_amb = v("reflection_tweak_interior_amb", 2);
    e.reflection_exterior_amb = v("reflection_tweak_exterior_amb", 2);
    e.reflection_emissive = v("reflection_tweak_emissive", 2);
    e.reflection_directional = v("reflection_tweak_directional", 0.3f);
    e.reflection_hdr = v("reflection_hdr_mult", 1);
    // the sky: sky_system.fxc's constants
    auto skycol = [&](const std::string& base) { return rgb(base) * v(base + "_inten", 1); };
    Sky& s = e.sky;
    s.azimuth_east = skycol("sky_azimuth_east_col");
    s.azimuth_west = skycol("sky_azimuth_west_col");
    s.azimuth_transition = skycol("sky_azimuth_transition_col");
    s.azimuth_transition_position = v("sky_azimuth_transition_position", 0.5f);
    s.zenith = skycol("sky_zenith_col");
    s.zenith_transition = skycol("sky_zenith_transition_col");
    s.zenith_transition_position = v("sky_zenith_transition_position", 0.3f);
    s.zenith_east_blend = v("sky_zenith_transition_east_blend", 0.8f);
    s.zenith_west_blend = v("sky_zenith_transition_west_blend", 0.8f);
    s.zenith_blend_start = v("sky_zenith_blend_start");
    s.hdr = v("sky_hdr", 1);
    s.sun_colour = rgb("sky_sun_col") * v("sky_sun_hdr");
    s.mie_phase = v("sky_sun_miephase", 0.95f);
    s.mie_scatter = v("sky_sun_miescatter", 1);
    s.mie_intensity = v("sky_sun_mie_intensity_mult", 0.2f);
    s.sun_disc = rgb("sky_sun_disc_col") * v("sky_sun_hdr");
    s.sun_disc_size = v("sky_sun_disc_size", 2);
    e.sun_shadow_distance = v("dir_shadow_distance_multiplier", 1);
    // the fog
    Fog& f = e.fogs;
    const float fog_hdr = v("fog_hdr", 1);
    f.start = v("fog_start");
    f.density = v("fog_density");
    f.falloff = v("fog_falloff");
    f.base_height = v("fog_base_height");
    f.alpha = v("fog_alpha", 1);
    f.near_colour = rgb("fog_near_col") * fog_hdr;
    f.colour = rgb("fog_col") * fog_hdr;
    f.east_colour = rgb("fog_east_col") * fog_hdr;
    f.moon_colour = rgb("fog_moon_col") * fog_hdr;
    f.sun_power = v("fog_sun_lighting_calc_pow", 8);
    f.moon_power = v("fog_moon_lighting_calc_pow", 18);
    f.haze_density = v("fog_haze_density");
    f.haze_alpha = v("fog_haze_alpha", 1);
    f.haze_start = v("fog_haze_start");
    f.haze_colour = rgb("fog_haze_col") * v("fog_haze_hdr", 1);
    e.vignette_intensity = v("postfx_vignetting_intensity");
    e.vignette_radius = v("postfx_vignetting_radius", 10);
    e.vignette_contrast = v("postfx_vignetting_contrast");
    e.vignette_colour = rgb("postfx_vignetting_col");
    e.gradient_top = rgb("postfx_grad_top_col", {1, 1, 1});
    e.gradient_middle = rgb("postfx_grad_middle_col", {1, 1, 1});
    e.gradient_bottom = rgb("postfx_grad_bottom_col", {1, 1, 1});
    e.gradient_midpoint = v("postfx_grad_midpoint", 0.5f);
    e.bloom_threshold = std::max(v("postfx_bright_pass_thresh", 0.4f), setting("bloom.threshold.min", 0.4f));
    e.bloom_threshold_width = v("postfx_bright_pass_thresh_width", 1);
    e.bloom_intensity = std::min(v("postfx_intensity_bloom", 0.2f), setting("misc.BloomIntensityClamp.HD", 10));
    e.bloom_blend[0] = setting("bloom.blurblendmult.small", 1);
    e.bloom_blend[1] = setting("bloom.blurblendmult.med", 2);
    e.bloom_blend[2] = setting("bloom.blurblendmult.large", 0.5f);
    return e;
}

} // namespace rk::render
