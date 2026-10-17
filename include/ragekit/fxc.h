#pragma once
// Compiled shader effects (.fxc, common.rpf/shaders/win32_40_final) and shader presets (.sps, common.rpf/shaders/db).
// read_fxc reads the effect's shaders (their DXBC bytecode), constant buffers, variables (type, register, array count,
// default values, UI annotations) and techniques. Layout from CodeWalker's FxcFile.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rk {

enum class FxcVarType : uint8_t {
    Float = 2, Float2 = 3, Float3 = 4, Float4 = 5,
    Texture = 6,   // a texture or a sampler
    Bool = 7, Float3x4 = 8, Float4x4 = 9, Int = 11, Int4 = 14, Buffer = 15,
    UavBuffer = 21, UavTexture = 22,
};

/// A UI annotation of a variable: UIName, UIWidget, UIMin, UIMax, UIStep, UIHint, ...
struct FxcAnnotation {
    std::string name;
    uint8_t type = 0;      // 0 int, 1 float, 2 string
    int32_t i = 0;
    float f = 0;
    std::string s;
};

struct FxcVariable {
    FxcVarType type = FxcVarType::Float;
    uint8_t count = 0;     // array length (0 = not an array)
    uint8_t slot = 0;      // register (samplers: the sampler slot)
    uint8_t group = 0;
    std::string name;      // e.g. "DiffuseSampler", "bumpiness"
    std::string name2;     // the semantic / second name (textures: "DiffuseTex"), often empty
    uint8_t offset = 0;    // offset in the constant buffer, in 16-byte registers
    uint8_t variant = 0;
    uint32_t cbuffer = 0;  // joaat of the lower-case name of the constant buffer it lives in; 0 = none (textures)
    std::vector<FxcAnnotation> annotations;
    std::vector<float> values_f;     // default value(s) of float types
    std::vector<uint32_t> values_u;  // default value(s) of int / bool / texture types
    const FxcAnnotation* annotation(std::string_view n) const;
};

struct FxcCBuffer {
    uint32_t size = 0;     // bytes
    uint16_t slot[6] = {}; // register per stage: VS PS CS DS GS HS
    std::string name;      // "rage_matrices", "<effect>_locals" (the per-material values), ...
    uint32_t name_hash = 0;
};

/// One compiled shader of an effect.
struct FxcShader {
    std::string name;                // "VS_TransformD", "PS_DeferredTextured" ...
    std::vector<uint8_t> bytecode;   // DXBC (D3DDisassemble reads it); empty for the "NULL" entry
};

/// Indices into FxcFile::shaders[stage]; 0 = none.
struct FxcPass { uint8_t vs = 0, ps = 0, cs = 0, ds = 0, gs = 0, hs = 0; };
struct FxcTechnique { std::string name; std::vector<FxcPass> passes; };

struct FxcFile {
    uint32_t vertex_type = 0;
    std::vector<std::pair<std::string, uint32_t>> preset_params;   // e.g. __rage_drawbucket 3
    std::vector<FxcCBuffer> cbuffers;
    std::vector<FxcVariable> variables;   // both blocks, in file order
    std::vector<FxcTechnique> techniques;
    std::array<std::vector<FxcShader>, 6> shaders;   // per stage (VS PS CS DS GS HS); entry 0 is "NULL"
    const FxcCBuffer* cbuffer(uint32_t name_hash) const;
};

/// Reads an .fxc ("rgxe"). Throws rk::Error when the data is not one.
FxcFile read_fxc(std::span<const uint8_t> data);

/// A shader preset (.sps): the base shader it instantiates and its draw bucket.
struct SpsPreset {
    std::string name;   // the file name without ".sps", e.g. "normal_spec_cutout"
    std::string base;   // "normal_spec"
    int bucket = 0;     // __rage_drawbucket (0 when the file names none)
};
/// Parses the text of a .sps file ("shader <base>", optionally "__rage_drawbucket { int N }").
SpsPreset read_sps(std::string_view text, const std::string& name);

}   // namespace rk
