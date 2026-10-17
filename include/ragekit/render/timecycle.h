// ragekit render - the game's timecycle: the weather files (common/data/timecycle/w_*.xml, named by weather.xml), the hours
// of their keyframes (time.xml) and the postfx defaults of visualsettings.dat, read into the Environment a scene is lit by.
//
// Each variable of a weather has one value per keyframe (time.xml's samples: 0, 5, 6, 7, 10, 12, 16 ... 22 h) per region
// (GLOBAL, URBAN); values between keyframes blend linearly, as CodeWalker's Timecycle.SetTime does.
#pragma once
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "ragekit/render/environment.h"

namespace rk {
class GameIndex;
}

namespace rk::render {

class Timecycle {
public:
    /// Reads the files through the index; throws rk::Error when time.xml or weather.xml is missing.
    static Timecycle load(const GameIndex& gi);

    /// The weathers, in weather.xml's order ("EXTRASUNNY", "CLEAR", ...).
    const std::vector<std::string>& weathers() const { return order_; }
    /// The keyframes' hours.
    const std::vector<float>& hours() const { return hours_; }

    /// One variable ("light_dir_mult") of a weather at an hour; `fallback` if either is unknown.
    float value(std::string_view weather, std::string_view variable, float hour, std::string_view region = "GLOBAL",
                float fallback = 0) const;
    /// The scene light of a weather (case-insensitive name; EXTRASUNNY if unknown) at an hour (0..24), with a timecycle
    /// modifier (an interior room's timecycleName, a zone's) applied at a strength 0..1.
    Environment environment(std::string_view weather, float hour, std::string_view region = "GLOBAL",
                            std::string_view modifier = {}, float strength = 1) const;
    /// The modifiers (timecycle_mods_*.xml of the base game, the update and the DLCs), lower-case names.
    std::vector<std::string> modifiers() const;
    bool has_modifier(std::string_view name) const;
    /// A value of visualsettings.dat ("Tonemapping.bright.filmic.A"), `fallback` if absent.
    float setting(std::string_view key, float fallback = 0) const;

private:
    struct Region { std::map<std::string, std::vector<float>, std::less<>> values; };
    struct Weather { std::map<std::string, Region, std::less<>> regions; };
    std::vector<float> hours_, durations_;
    float sun_roll_ = 122, moon_roll_ = -122;
    std::map<std::string, Weather, std::less<>> weathers_;
    std::vector<std::string> order_;
    std::map<std::string, float, std::less<>> settings_;
    // a modifier's variables: the value it sets (applied as lerp(base, value, strength)); the second number of each pair
    // (1 on colours, 0 on most scalars) is kept but not applied: what the game does with it is not known
    struct Modifier { std::map<std::string, std::pair<float, float>, std::less<>> values; };
    std::map<std::string, Modifier, std::less<>> modifiers_;
};

} // namespace rk::render
