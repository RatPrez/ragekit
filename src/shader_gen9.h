// ragekit internal - the shader table: for every shader of the game's drawables, its Enhanced parameter layout (with the
// values the game's files hold most), the Legacy parameters (order, register slots, defaults), how the names of the two
// editions correspond, and the presets. src/shader_gen9.inc holds it, made by tools/gen_shader_table.cpp from a Legacy and
// an Enhanced install.
#pragma once
#include <cstdint>
#include <vector>
#include "ragekit/drawable.h"

namespace rk::shader_table {

/// The Enhanced layout of a shader, its buffers holding the table's values and no textures; null when the table does not know it.
const ShaderGen9* layout(uint32_t shader);
/// The Enhanced name of a shader's Legacy parameter; 0 when Enhanced has no such parameter or the table does not know the shader.
uint32_t gen9_name(uint32_t shader, uint32_t legacy_param);
/// The Legacy name of a shader's Enhanced parameter; 0 when Legacy has no such parameter.
uint32_t legacy_name(uint32_t shader, uint32_t gen9_param);
/// The Legacy parameters of a shader, in Legacy's order with its slots and the effect's defaults; empty when the table does not know it.
std::vector<ShaderParam> legacy_params(uint32_t shader);
/// The preset (.sps hash) the game's Legacy drawables use for a shader drawn in a bucket, and their unk12; false when unknown.
bool preset(uint32_t shader, uint8_t bucket, uint32_t& sps, uint16_t& unk12);

/// The vertex types the game's Legacy drawables use for a shader and semantics when they are not the standard table's
/// (cloth: half floats, signed normals); false otherwise.
bool legacy_layout(uint32_t shader, uint32_t flags, uint64_t& types);

/// The Legacy view of an Enhanced shader: Shader::params made from `g` (Legacy names, order, slots; the table's defaults for
/// Legacy parameters Enhanced lacks). A shader the table does not know keeps the Enhanced names, in the table's order.
std::vector<ShaderParam> params_from_gen9(uint32_t shader, const ShaderGen9& g);
/// The Enhanced data of a shader: `s.gen9`, else the table's layout, with the values of `s.params` written into it.
/// Throws rk::Error when neither exists.
ShaderGen9 gen9_for(const Shader& s);

}   // namespace rk::shader_table
