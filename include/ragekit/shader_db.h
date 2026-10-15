#pragma once
// The shaders of the game: every base shader (from its .fxc) with the material parameters a drawable carries, every
// preset (.sps) with its base shader and draw bucket. Built from a game install (GameIndex) and cached as text.
//
// What a drawable's shader holds, measured on the game's own drawables: the effect's material textures (samplers with a
// UIName / TextureType annotation) in file order with their sampler slot, then the variables of its "*_locals" constant
// buffers in REVERSE file order with slots 160 + n - 1 down to 160. instantiate() writes exactly that.
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "ragekit/drawable.h"
#include "ragekit/types.h"

namespace rk {

class GameIndex;

struct ShaderParamDef {
    std::string name;            // "DiffuseSampler", "bumpiness" (the drawable stores joaat_lower(name))
    bool texture = false;
    uint8_t components = 4;      // vectors: 1..4 floats used (stored as a Vec4)
    uint8_t count = 0;           // vectors: array length (0 = a single value)
    uint8_t slot = 0;            // textures: the sampler slot the game writes
    Vec4 value{0, 0, 0, 0};      // the effect's default (vectors)
    std::string ui_name;         // "Specular Intensity" (empty: none given)
    std::string texture_type;    // textures: "Diffuse", "Normal", "Specular", ... (TextureType / TCPTemplate)
    std::string hint;            // UIHint ("normalmap", "specularmap", ...)
    float ui_min = 0, ui_max = 0, ui_step = 0;   // 0 / 0 = not given
};

struct ShaderBaseDef {
    std::string name;            // "normal_spec"
    uint32_t vertex_type = 0;    // the effect's vertex declaration id (the geometry layout it expects)
    std::vector<ShaderParamDef> textures;   // file order
    std::vector<ShaderParamDef> vectors;    // file order (the drawable stores them reversed)
};

struct ShaderPresetDef {
    std::string name;            // "normal_spec_cutout" (the .sps name without the extension)
    std::string base;            // "normal_spec"
    int bucket = 0;              // render bucket: 0 opaque, 1 alpha, 2 decal, 3 cutout, ...
};

class ShaderDb {
public:
    /// Reads every .sps of shaders/db and every .fxc of shaders/win32_40_final from the install. Throws rk::Error when
    /// the install has none.
    static ShaderDb from_game(const GameIndex& gi);
    /// The text cache written by save(); empty on failure.
    static ShaderDb load(const std::string& path);
    bool save(const std::string& path) const;

    bool empty() const { return bases_.empty(); }
    size_t base_count() const { return bases_.size(); }
    size_t preset_count() const { return presets_.size(); }
    const ShaderBaseDef* base(const std::string& name) const;
    const ShaderPresetDef* preset(const std::string& name) const;
    /// The preset that draws `base` in `bucket` (normal_spec + 3 -> normal_spec_cutout); the base's own preset first.
    const ShaderPresetDef* preset_for(const std::string& base, int bucket) const;
    std::vector<std::string> base_names() const;
    std::vector<std::string> preset_names() const;

    /// A drawable shader for the preset: Name = the base shader, sps = the preset, its bucket, every material parameter
    /// in the game's order and slots with the effect's defaults; `textures` binds sampler name -> texture name.
    Shader instantiate(const ShaderPresetDef& preset, const std::map<std::string, std::string>& textures = {}) const;

    ShaderBaseDef& add_base(ShaderBaseDef b);
    ShaderPresetDef& add_preset(ShaderPresetDef p);

private:
    std::map<std::string, ShaderBaseDef> bases_;
    std::map<std::string, ShaderPresetDef> presets_;
};

}   // namespace rk
