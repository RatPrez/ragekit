#include "ragekit/shader_db.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include "ragekit/error.h"
#include "ragekit/fxc.h"
#include "ragekit/gameindex.h"
#include "ragekit/hash.h"

namespace rk {
namespace {

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// cache text: one token per field, strings with spaces percent-escaped
std::string esc(const std::string& s) {
    if (s.empty()) return "-";
    std::string o;
    for (char c : s) {
        if (c == ' ' || c == '%' || c == '\n' || c == '\r' || c == '\t') { char b[4]; std::snprintf(b, sizeof b, "%%%02X", (unsigned char)c); o += b; }
        else o += c;
    }
    return o;
}
std::string unesc(const std::string& s) {
    if (s == "-") return {};
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) { o += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else o += s[i];
    }
    return o;
}

ShaderParamDef param_from(const FxcVariable& v) {
    ShaderParamDef p;
    p.name = v.name;
    p.texture = v.type == FxcVarType::Texture;
    switch (v.type) {
    case FxcVarType::Float: case FxcVarType::Int: case FxcVarType::Bool: p.components = 1; break;
    case FxcVarType::Float2: p.components = 2; break;
    case FxcVarType::Float3: p.components = 3; break;
    default: p.components = 4; break;
    }
    p.count = v.count;
    p.slot = v.slot;
    float d[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < 4 && i < v.values_f.size(); i++) d[i] = v.values_f[i];
    if (v.values_f.empty() && !v.values_u.empty() && !p.texture) d[0] = (float)v.values_u[0];
    p.value = {d[0], d[1], d[2], d[3]};
    for (const FxcAnnotation& a : v.annotations) {
        if (a.name == "UIName" && a.type == 2) p.ui_name = a.s;
        else if ((a.name == "TextureType" || (a.name == "TCPTemplate" && p.texture_type.empty())) && a.type == 2) p.texture_type = a.s;
        else if (a.name == "UIHint" && a.type == 2) p.hint = a.s;
        else if (a.name == "UIMin") p.ui_min = a.type == 1 ? a.f : (float)a.i;
        else if (a.name == "UIMax") p.ui_max = a.type == 1 ? a.f : (float)a.i;
        else if (a.name == "UIStep") p.ui_step = a.type == 1 ? a.f : (float)a.i;
    }
    return p;
}

}   // namespace

ShaderBaseDef& ShaderDb::add_base(ShaderBaseDef b) { std::string k = b.name; return bases_[k] = std::move(b); }
ShaderPresetDef& ShaderDb::add_preset(ShaderPresetDef p) { std::string k = p.name; return presets_[k] = std::move(p); }

const ShaderBaseDef* ShaderDb::base(const std::string& name) const { auto it = bases_.find(lower(name)); return it == bases_.end() ? nullptr : &it->second; }
const ShaderPresetDef* ShaderDb::preset(const std::string& name) const {
    std::string n = lower(name);
    if (ends_with(n, ".sps")) n.resize(n.size() - 4);
    auto it = presets_.find(n);
    return it == presets_.end() ? nullptr : &it->second;
}

const ShaderPresetDef* ShaderDb::preset_for(const std::string& base_name, int bucket) const {
    const std::string b = lower(base_name);
    if (const ShaderPresetDef* own = preset(b); own && own->base == b && own->bucket == bucket) return own;
    for (const auto& [n, p] : presets_) if (p.base == b && p.bucket == bucket) return &p;
    return nullptr;
}

std::vector<std::string> ShaderDb::base_names() const { std::vector<std::string> v; for (auto& [n, b] : bases_) v.push_back(n); return v; }
std::vector<std::string> ShaderDb::preset_names() const { std::vector<std::string> v; for (auto& [n, p] : presets_) v.push_back(n); return v; }

Shader ShaderDb::instantiate(const ShaderPresetDef& pr, const std::map<std::string, std::string>& textures) const {
    Shader s;
    s.name_hash = joaat_lower(pr.base);
    s.sps_hash = joaat_lower(pr.name + ".sps");
    s.render_bucket = (uint8_t)std::clamp(pr.bucket, 0, 7);
    const ShaderBaseDef* b = base(pr.base);
    if (!b) return s;
    for (const ShaderParamDef& t : b->textures) {
        auto it = textures.find(t.name);
        ShaderParam p = ShaderParam::tex(t.name, it == textures.end() ? std::string() : it->second);
        p.slot = t.slot;
        s.params.push_back(std::move(p));
    }
    // vectors: reverse file order, slots 160 + n - 1 down to 160; an array takes `count` consecutive values
    const size_t n = b->vectors.size();
    for (size_t k = 0; k < n; k++) {
        const ShaderParamDef& v = b->vectors[n - 1 - k];
        ShaderParam p;
        p.name_hash = joaat_lower(v.name);
        p.type = v.count > 0 ? ParamType::Array : ParamType::Vector;
        p.values.assign(v.count > 0 ? v.count : 1, v.value);
        p.slot = (uint8_t)(160 + (n - 1 - k));
        s.params.push_back(std::move(p));
    }
    return s;
}

ShaderDb ShaderDb::from_game(const GameIndex& gi) {
    ShaderDb db;
    for (const IndexedFile& f : gi.files()) {
        const std::string ext = gi.extension(f);
        if (ext != ".sps" && ext != ".fxc") continue;
        const std::string path = lower(gi.path(f));
        std::string name = gi.file_name(f);
        name.resize(name.size() - ext.size());
        try {
            if (ext == ".sps" && path.find("shaders/db/") != std::string::npos) {
                Bytes b = gi.extract(f);
                SpsPreset s = read_sps(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), name);
                if (!s.base.empty()) db.add_preset({lower(s.name), lower(s.base), s.bucket});
            } else if (ext == ".fxc" && path.find("win32_40_final") != std::string::npos) {
                Bytes b = gi.extract(f);
                FxcFile fx = read_fxc(b);
                ShaderBaseDef base;
                base.name = lower(name);
                base.vertex_type = fx.vertex_type;
                for (const FxcVariable& v : fx.variables) {
                    if (v.type == FxcVarType::Texture) {
                        // the material's own textures carry a UI annotation; the engine's (shadow maps, gbuffers) do not
                        if (v.annotation("UIName") || v.annotation("TextureType")) base.textures.push_back(param_from(v));
                        continue;
                    }
                    const FxcCBuffer* cb = fx.cbuffer(v.cbuffer);
                    if (cb && ends_with(lower(cb->name), "_locals")) base.vectors.push_back(param_from(v));
                }
                db.add_base(std::move(base));
            }
        } catch (const std::exception&) {
            // a damaged or unknown file: leave it out
        }
    }
    if (db.bases_.empty()) throw Error("no shader effects (.fxc) found in the game install");
    return db;
}

bool ShaderDb::save(const std::string& path) const {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    o << "rk-shaderdb 1\n";
    for (const auto& [n, p] : presets_) o << "preset " << esc(p.name) << " " << esc(p.base) << " " << p.bucket << "\n";
    for (const auto& [n, b] : bases_) {
        o << "base " << esc(b.name) << " " << b.vertex_type << "\n";
        for (int pass = 0; pass < 2; pass++)
            for (const ShaderParamDef& p : pass == 0 ? b.textures : b.vectors)
                o << (p.texture ? "tex " : "vec ") << esc(p.name) << " " << (int)p.components << " " << (int)p.count << " " << (int)p.slot << " "
                  << p.value.x << " " << p.value.y << " " << p.value.z << " " << p.value.w << " " << p.ui_min << " " << p.ui_max << " " << p.ui_step << " "
                  << esc(p.ui_name) << " " << esc(p.texture_type) << " " << esc(p.hint) << "\n";
    }
    return (bool)o;
}

ShaderDb ShaderDb::load(const std::string& path) {
    ShaderDb db;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line) || line.rfind("rk-shaderdb 1", 0) != 0) return db;
    ShaderBaseDef* cur = nullptr;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream s(line);
        std::string key;
        s >> key;
        if (key == "preset") {
            ShaderPresetDef p; std::string a, b;
            s >> a >> b >> p.bucket;
            p.name = unesc(a); p.base = unesc(b);
            db.add_preset(p);
        } else if (key == "base") {
            ShaderBaseDef b; std::string a;
            s >> a >> b.vertex_type;
            b.name = unesc(a);
            cur = &db.add_base(std::move(b));
        } else if ((key == "tex" || key == "vec") && cur) {
            ShaderParamDef p; std::string n, ui, tt, hint; int comp = 4, count = 0, slot = 0;
            s >> n >> comp >> count >> slot >> p.value.x >> p.value.y >> p.value.z >> p.value.w >> p.ui_min >> p.ui_max >> p.ui_step >> ui >> tt >> hint;
            p.name = unesc(n); p.ui_name = unesc(ui); p.texture_type = unesc(tt); p.hint = unesc(hint);
            p.texture = key == "tex"; p.components = (uint8_t)comp; p.count = (uint8_t)count; p.slot = (uint8_t)slot;
            (p.texture ? cur->textures : cur->vectors).push_back(std::move(p));
        }
    }
    return db;
}

}   // namespace rk
