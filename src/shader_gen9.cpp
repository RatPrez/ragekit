#include "shader_gen9.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <mutex>
#include "ragekit/error.h"
#include "ragekit/hash.h"

namespace rk {

const ShaderParamGen9* ShaderGen9::find(uint32_t h) const {
    for (auto& p : params) if (p.name_hash == h) return &p;
    return nullptr;
}

uint32_t ShaderGen9::buffer_offset(uint32_t buffer) const {
    uint32_t o = 0;
    for (uint32_t i = 0; i < buffer && i < buffer_sizes.size(); i++) o += buffer_sizes[i];
    return o;
}

namespace shader_table {

namespace data {
struct ShaderRow {
    uint32_t name;
    uint16_t params, nparams;     // kParams
    uint16_t buffers, nbuffers;   // kBufferSizes
    uint16_t samplers, nsamplers; // kSamplers
    uint32_t defaults;            // kDefaults: the buffers' contents, as 32-bit words
    uint16_t legacy, nlegacy;     // kLegacy
    uint16_t presets, npresets;   // kPresets
};
struct ParamRow { uint32_t gen9, data, legacy; };                  // legacy: 0 when Legacy has no such parameter
struct LegacyRow { uint32_t name; uint8_t count, slot; uint16_t values; };   // count 0: a texture; values: kFloats / 4 (count Vec4)
struct PresetRow { uint32_t sps; uint8_t bucket; uint16_t unk12; };
struct LayoutRow { uint32_t shader, flags; uint64_t types; };                  // sorted by (shader, flags)
#include "shader_gen9.inc"
}   // namespace data

namespace {

const data::ShaderRow* row(uint32_t shader) {
    const data::ShaderRow* b = std::begin(data::kShaders);
    const data::ShaderRow* e = std::end(data::kShaders);
    auto it = std::lower_bound(b, e, shader, [](const data::ShaderRow& r, uint32_t h) { return r.name < h; });
    return it != e && it->name == shader ? it : nullptr;
}

// Floats as stored in a constant: 4 bytes per float, at most `length` bytes, from the parameter's Vec4 values.
void put_values(Bytes& buf, size_t at, size_t length, const std::vector<Vec4>& values) {
    std::vector<float> f;
    for (auto& v : values) { f.push_back(v.x); f.push_back(v.y); f.push_back(v.z); f.push_back(v.w); }
    const size_t n = std::min(length, f.size() * 4);
    if (at + n > buf.size()) return;
    std::memcpy(buf.data() + at, f.data(), n);
}

std::vector<Vec4> get_values(const Bytes& buf, size_t at, size_t length) {
    std::vector<Vec4> out(std::max<size_t>(1, length / 16));
    if (length <= 16) {
        float f[4] = {0, 0, 0, 0};
        if (at + length <= buf.size()) std::memcpy(f, buf.data() + at, length);
        out[0] = {f[0], f[1], f[2], f[3]};
    } else if (at + out.size() * 16 <= buf.size()) {
        std::memcpy(out.data(), buf.data() + at, out.size() * 16);
    }
    return out;
}

}   // namespace

const ShaderGen9* layout(uint32_t shader) {
    static std::mutex mu;
    static std::map<uint32_t, ShaderGen9> made;
    const data::ShaderRow* r = row(shader);
    if (!r) return nullptr;
    std::lock_guard<std::mutex> g(mu);
    auto it = made.find(shader);
    if (it != made.end()) return &it->second;
    ShaderGen9 l;
    uint32_t textures = 0, unknowns = 0;
    for (uint32_t i = 0; i < r->nparams; i++) {
        const auto& p = data::kParams[r->params + i];
        ShaderParamGen9 sp{p.gen9, p.data};
        l.params.push_back(sp);
        if (sp.kind() == ShaderParamGen9::Texture) textures = std::max<uint32_t>(textures, sp.slot() + 1u);
        if (sp.kind() == ShaderParamGen9::Unknown) unknowns = std::max<uint32_t>(unknowns, sp.slot() + 1u);
    }
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < r->nbuffers; i++) { l.buffer_sizes.push_back(data::kBufferSizes[r->buffers + i]); bytes += l.buffer_sizes.back(); }
    l.buffers.resize(bytes);
    if (bytes) std::memcpy(l.buffers.data(), &data::kDefaults[r->defaults], bytes);
    l.samplers.assign(&data::kSamplers[r->samplers], &data::kSamplers[r->samplers] + r->nsamplers);
    l.textures.resize(textures);
    l.unknowns.resize(unknowns);
    return &made.emplace(shader, std::move(l)).first->second;
}

uint32_t gen9_name(uint32_t shader, uint32_t legacy_param) {
    const data::ShaderRow* r = row(shader);
    if (!r || !legacy_param) return 0;
    for (uint32_t i = 0; i < r->nparams; i++)
        if (data::kParams[r->params + i].legacy == legacy_param) return data::kParams[r->params + i].gen9;
    return 0;
}

uint32_t legacy_name(uint32_t shader, uint32_t gen9_param) {
    const data::ShaderRow* r = row(shader);
    if (!r) return 0;
    for (uint32_t i = 0; i < r->nparams; i++)
        if (data::kParams[r->params + i].gen9 == gen9_param) return data::kParams[r->params + i].legacy;
    return 0;
}

std::vector<ShaderParam> legacy_params(uint32_t shader) {
    std::vector<ShaderParam> out;
    const data::ShaderRow* r = row(shader);
    if (!r) return out;
    for (uint32_t i = 0; i < r->nlegacy; i++) {
        const auto& l = data::kLegacy[r->legacy + i];
        ShaderParam p;
        p.name_hash = l.name;
        p.slot = l.slot;
        if (l.count == 0) {
            p.type = ParamType::Texture;
        } else {
            p.type = l.count == 1 ? ParamType::Vector : ParamType::Array;
            for (uint32_t k = 0; k < l.count; k++) {
                const float* f = &data::kFloats[(size_t)(l.values + k) * 4];
                p.values.push_back({f[0], f[1], f[2], f[3]});
            }
        }
        out.push_back(std::move(p));
    }
    return out;
}

bool legacy_layout(uint32_t shader, uint32_t flags, uint64_t& types) {
    for (const auto& l : data::kLayouts)
        if (l.shader == shader && l.flags == flags && l.types) { types = l.types; return true; }
    return false;
}

bool preset(uint32_t shader, uint8_t bucket, uint32_t& sps, uint16_t& unk12) {
    const data::ShaderRow* r = row(shader);
    if (!r) return false;
    for (uint32_t i = 0; i < r->npresets; i++) {
        const auto& p = data::kPresets[r->presets + i];
        if (p.bucket == bucket) { sps = p.sps; unk12 = p.unk12; return true; }
    }
    return false;
}

std::vector<ShaderParam> params_from_gen9(uint32_t shader, const ShaderGen9& g) {
    std::vector<ShaderParam> out = legacy_params(shader);
    if (out.empty()) {
        // the table does not know the shader: its Enhanced parameters under their Enhanced names
        for (auto& p : g.params) {
            ShaderParam sp;
            sp.name_hash = p.name_hash;
            if (p.kind() == ShaderParamGen9::Texture) {
                sp.type = ParamType::Texture;
                if (p.slot() < g.textures.size()) sp.texture = g.textures[p.slot()];
            } else if (p.kind() == ShaderParamGen9::Constant) {
                sp.values = get_values(g.buffers, g.buffer_offset(p.slot()) + p.offset(), p.length());
                sp.type = sp.values.size() > 1 ? ParamType::Array : ParamType::Vector;
            } else {
                continue;
            }
            out.push_back(std::move(sp));
        }
        return out;
    }
    for (auto& sp : out) {
        const uint32_t n9 = gen9_name(shader, sp.name_hash);
        const ShaderParamGen9* p = n9 ? g.find(n9) : nullptr;
        if (!p) continue;   // Enhanced has no such parameter: the effect's default
        if (sp.type == ParamType::Texture && p->kind() == ShaderParamGen9::Texture) {
            sp.texture = p->slot() < g.textures.size() ? g.textures[p->slot()] : "";
        } else if (sp.type != ParamType::Texture && p->kind() == ShaderParamGen9::Constant) {
            std::vector<Vec4> v = get_values(g.buffers, g.buffer_offset(p->slot()) + p->offset(), p->length());
            // the Enhanced value replaces as many floats of the Legacy default as it holds
            std::vector<float> f;
            for (auto& d : sp.values) { f.push_back(d.x); f.push_back(d.y); f.push_back(d.z); f.push_back(d.w); }
            std::vector<float> n;
            for (auto& d : v) { n.push_back(d.x); n.push_back(d.y); n.push_back(d.z); n.push_back(d.w); }
            for (size_t k = 0; k < f.size() && k < (size_t)p->length() / 4; k++) f[k] = n[k];
            for (size_t k = 0; k < sp.values.size(); k++) sp.values[k] = {f[k * 4], f[k * 4 + 1], f[k * 4 + 2], f[k * 4 + 3]};
        }
    }
    return out;
}

ShaderGen9 gen9_for(const Shader& s) {
    ShaderGen9 g;
    if (s.gen9) g = *s.gen9;
    else if (const ShaderGen9* l = layout(s.name_hash)) g = *l;
    else throw Error("shader " + hash_name(s.name_hash) + " has no Enhanced parameter layout (the shader table knows the shaders of the game's drawables)");
    for (auto& sp : s.params) {
        uint32_t n9 = gen9_name(s.name_hash, sp.name_hash);
        if (!n9) n9 = sp.name_hash;   // already an Enhanced name (a shader the table does not know)
        const ShaderParamGen9* p = g.find(n9);
        if (!p) continue;
        if (sp.type == ParamType::Texture && p->kind() == ShaderParamGen9::Texture) {
            if (p->slot() >= g.textures.size()) g.textures.resize(p->slot() + 1u);
            g.textures[p->slot()] = sp.texture;
        } else if (sp.type != ParamType::Texture && p->kind() == ShaderParamGen9::Constant) {
            put_values(g.buffers, g.buffer_offset(p->slot()) + p->offset(), p->length(), sp.values);
        }
    }
    return g;
}

}   // namespace shader_table
}   // namespace rk
