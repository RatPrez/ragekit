#include "ragekit/fxc.h"
#include <cctype>
#include <cstring>
#include "ragekit/error.h"
#include "ragekit/hash.h"

namespace rk {
namespace {

struct In {
    std::span<const uint8_t> d;
    size_t p = 0;
    void need(size_t n) const { if (p + n > d.size()) throw Error("fxc: truncated at " + std::to_string(p)); }
    uint8_t u8() { need(1); return d[p++]; }
    uint16_t u16() { need(2); uint16_t v; std::memcpy(&v, d.data() + p, 2); p += 2; return v; }
    uint32_t u32() { need(4); uint32_t v; std::memcpy(&v, d.data() + p, 4); p += 4; return v; }
    float f32() { need(4); float v; std::memcpy(&v, d.data() + p, 4); p += 4; return v; }
    void skip(size_t n) { need(n); p += n; }
    // a length-prefixed string; the length counts the terminating zero
    std::string str() {
        const uint8_t n = u8();
        if (n == 0) return {};
        need(n);
        std::string s(reinterpret_cast<const char*>(d.data() + p), n > 1 ? n - 1 : 0);
        p += n;
        return s;
    }
};

void read_shader(In& in, int stage) {
    std::string name = in.str();
    if (name.empty()) name = in.str();   // geometry shaders sometimes carry an extra empty string first
    const uint8_t nvars = in.u8();
    for (int i = 0; i < nvars; i++) in.str();
    const uint8_t nbuf = in.u8();
    for (int i = 0; i < nbuf; i++) { in.str(); in.u16(); }
    if (stage == 4) in.u8();             // geometry shaders: one more byte
    const uint32_t len = in.u32();
    if (len > 0) {
        in.skip(len);                    // the DXBC bytecode
        if (stage == 0 || stage == 1 || stage == 4) in.skip(2);   // shader model major / minor (VS, PS, GS)
    }
}

FxcVariable read_variable(In& in) {
    FxcVariable v;
    v.type = (FxcVarType)in.u8();
    v.count = in.u8();
    v.slot = in.u8();
    v.group = in.u8();
    v.name = in.str();
    v.name2 = in.str();
    v.offset = in.u8();
    v.variant = in.u8();
    in.u8(); in.u8();                    // always 0
    v.cbuffer = in.u32();
    const uint8_t np = in.u8();
    for (int i = 0; i < np; i++) {
        FxcAnnotation a;
        a.name = in.str();
        a.type = in.u8();
        if (a.type == 0) a.i = (int32_t)in.u32();
        else if (a.type == 1) a.f = in.f32();
        else if (a.type == 2) a.s = in.str();
        v.annotations.push_back(std::move(a));
    }
    const uint8_t nv = in.u8();
    const bool ints = v.type == FxcVarType::Int || v.type == FxcVarType::Int4 || v.type == FxcVarType::Bool || v.type == FxcVarType::Texture ||
                      v.type == FxcVarType::Buffer || v.type == FxcVarType::UavBuffer || v.type == FxcVarType::UavTexture;
    for (int i = 0; i < nv; i++) {
        if (ints) v.values_u.push_back(in.u32());
        else v.values_f.push_back(in.f32());
    }
    return v;
}

FxcCBuffer read_cbuffer(In& in) {
    FxcCBuffer c;
    c.size = in.u32();
    for (auto& s : c.slot) s = in.u16();
    c.name = in.str();
    c.name_hash = joaat_lower(c.name);
    return c;
}

}   // namespace

const FxcAnnotation* FxcVariable::annotation(std::string_view n) const {
    for (const FxcAnnotation& a : annotations) if (a.name == n) return &a;
    return nullptr;
}

const FxcCBuffer* FxcFile::cbuffer(uint32_t name_hash) const {
    for (const FxcCBuffer& c : cbuffers) if (c.name_hash == name_hash) return &c;
    return nullptr;
}

FxcFile read_fxc(std::span<const uint8_t> data) {
    In in{data};
    if (in.u32() != 0x65786772u) throw Error("fxc: not an effect (no \"rgxe\")");   // "rgxe"
    FxcFile f;
    f.vertex_type = in.u32();
    const uint8_t npp = in.u8();
    for (int i = 0; i < npp; i++) {
        std::string n = in.str();
        in.u8();   // always 0
        f.preset_params.push_back({n, in.u32()});
    }
    for (int stage = 0; stage < 6; stage++) {   // VS PS CS DS GS HS
        uint8_t count = in.u8();
        if (count == 0) count = in.u8();   // hull shaders: sometimes a byte more
        in.str();                          // "NULL"
        in.u8(); in.u8(); in.u32();
        for (int i = 1; i < count; i++) read_shader(in, stage);
    }
    for (int block = 0; block < 2; block++) {   // the constant buffers and variables, then more buffers and the textures / samplers
        const uint8_t ncb = in.u8();
        for (int i = 0; i < ncb; i++) f.cbuffers.push_back(read_cbuffer(in));
        const uint8_t nvar = in.u8();
        for (int i = 0; i < nvar; i++) f.variables.push_back(read_variable(in));
    }
    const uint8_t ntech = in.u8();
    for (int i = 0; i < ntech; i++) {
        FxcTechnique t;
        t.name = in.str();
        const uint8_t npass = in.u8();
        for (int k = 0; k < npass; k++) {
            FxcPass p;
            p.vs = in.u8(); p.ps = in.u8(); p.cs = in.u8(); p.ds = in.u8(); p.gs = in.u8(); p.hs = in.u8();
            const uint8_t nparam = in.u8();
            in.skip((size_t)nparam * 8);
            t.passes.push_back(p);
        }
        f.techniques.push_back(std::move(t));
    }
    return f;
}

SpsPreset read_sps(std::string_view text, const std::string& name) {
    SpsPreset s;
    s.name = name;
    auto word_after = [&](std::string_view key, size_t from) -> std::pair<std::string, size_t> {
        size_t p = text.find(key, from);
        if (p == std::string_view::npos) return {{}, std::string_view::npos};
        p += key.size();
        while (p < text.size() && std::isspace((unsigned char)text[p])) p++;
        size_t e = p;
        while (e < text.size() && !std::isspace((unsigned char)text[e]) && text[e] != '}') e++;
        return {std::string(text.substr(p, e - p)), e};
    };
    s.base = word_after("shader", 0).first;
    auto db = text.find("__rage_drawbucket");
    if (db != std::string_view::npos) {
        auto [num, end] = word_after("int", db);
        if (!num.empty()) s.bucket = std::atoi(num.c_str());
    }
    return s;
}

}   // namespace rk
