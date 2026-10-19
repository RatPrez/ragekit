// ragekit - CodeWalker XML writers (see xml.h). The reference is CodeWalker.Core's
//   Drawable.WriteXml / DrawableBase.WriteXml / ShaderGroup / ShaderFX / ShaderParametersBlock / Skeleton / Bone /
//   Joints / DrawableModel / DrawableGeometry / VertexBuffer / VertexData / IndexBuffer / LightAttributes
//   (GameFiles/Resources/Drawable.cs), Bounds.WriteXml and subclasses (Bounds.cs), TextureDictionary / Texture
//   (Texture.cs) and the Ydr/Ydd/Ybn/Ytd Xml classes; element order, tag names, indentation (one space per level),
//   self-closing rules and number formatting are copied from there.
#include "ragekit/xml.h"
#include "ragekit/hash.h"
#include "ragekit/image.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rk {

namespace {

struct NameEntry { uint32_t hash; const char* name; };
const NameEntry kParamNames[] = {
#include "xml_param_names.inc"
};
const NameEntry kShaderNames[] = {
#include "xml_shader_names.inc"
};

const char* lookup(const NameEntry* t, size_t n, uint32_t h) {
    auto it = std::lower_bound(t, t + n, h, [](const NameEntry& e, uint32_t v) { return e.hash < v; });
    return it != t + n && it->hash == h ? it->name : nullptr;
}

// ---------------------------------------------------------------- flag enums (.NET Enum.ToString for [Flags])

struct FlagName { uint32_t v; const char* n; };

std::string flags_string(uint32_t value, const FlagName* t, size_t n, const char* zero_name) {
    if (value == 0) return zero_name ? zero_name : "0";
    uint32_t rest = value;
    std::vector<const char*> found;
    for (size_t i = n; i-- > 0;) {
        if (t[i].v != 0 && (rest & t[i].v) == t[i].v) { rest &= ~t[i].v; found.push_back(t[i].n); }
    }
    if (rest != 0) return std::to_string(value);
    std::string out;
    for (size_t i = found.size(); i-- > 0;) { if (!out.empty()) out += ", "; out += found[i]; }
    return out;
}

#define FL(bit, name) {1u << (bit), name}
const FlagName kBoneFlags[] = {FL(0, "RotX"), FL(1, "RotY"), FL(2, "RotZ"), FL(3, "LimitRotation"), FL(4, "TransX"), FL(5, "TransY"),
    FL(6, "TransZ"), FL(7, "LimitTranslation"), FL(8, "ScaleX"), FL(9, "ScaleY"), FL(10, "ScaleZ"), FL(11, "LimitScale"),
    FL(12, "Unk0"), FL(13, "Unk1"), FL(14, "Unk2"), FL(15, "Unk3")};
const FlagName kTextureUsageFlags[] = {FL(0, "NOT_HALF"), FL(1, "HD_SPLIT"), FL(2, "X2"), FL(3, "X4"), FL(4, "Y4"), FL(5, "X8"), FL(6, "X16"),
    FL(7, "X32"), FL(8, "X64"), FL(9, "Y64"), FL(10, "X128"), FL(11, "X256"), FL(12, "X512"), FL(13, "Y512"), FL(14, "X1024"),
    FL(15, "Y1024"), FL(16, "X2048"), FL(17, "Y2048"), FL(18, "EMBEDDEDSCRIPTRT"), FL(19, "UNK19"), FL(20, "UNK20"), FL(21, "UNK21"),
    FL(22, "FLAG_FULL"), FL(23, "MAPS_HALF"), FL(24, "UNK24")};
const FlagName kMaterialFlags[] = {FL(0, "FLAG_STAIRS"), FL(1, "FLAG_NOT_CLIMBABLE"), FL(2, "FLAG_SEE_THROUGH"), FL(3, "FLAG_SHOOT_THROUGH"),
    FL(4, "FLAG_NOT_COVER"), FL(5, "FLAG_WALKABLE_PATH"), FL(6, "FLAG_NO_CAM_COLLISION"), FL(7, "FLAG_SHOOT_THROUGH_FX"),
    FL(8, "FLAG_NO_DECAL"), FL(9, "FLAG_NO_NAVMESH"), FL(10, "FLAG_NO_RAGDOLL"), FL(11, "FLAG_VEHICLE_WHEEL"), FL(12, "FLAG_NO_PTFX"),
    FL(13, "FLAG_TOO_STEEP_FOR_PLAYER"), FL(14, "FLAG_NO_NETWORK_SPAWN"), FL(15, "FLAG_NO_CAM_COLLISION_ALLOW_CLIPPING")};
const FlagName kCompositeFlags[] = {FL(0, "UNKNOWN"), FL(1, "MAP_WEAPON"), FL(2, "MAP_DYNAMIC"), FL(3, "MAP_ANIMAL"), FL(4, "MAP_COVER"),
    FL(5, "MAP_VEHICLE"), FL(6, "VEHICLE_NOT_BVH"), FL(7, "VEHICLE_BVH"), FL(8, "VEHICLE_BOX"), FL(9, "PED"), FL(10, "RAGDOLL"),
    FL(11, "ANIMAL"), FL(12, "ANIMAL_RAGDOLL"), FL(13, "OBJECT"), FL(14, "OBJECT_ENV_CLOTH"), FL(15, "PLANT"), FL(16, "PROJECTILE"),
    FL(17, "EXPLOSION"), FL(18, "PICKUP"), FL(19, "FOLIAGE"), FL(20, "FORKLIFT_FORKS"), FL(21, "TEST_WEAPON"), FL(22, "TEST_CAMERA"),
    FL(23, "TEST_AI"), FL(24, "TEST_SCRIPT"), FL(25, "TEST_VEHICLE_WHEEL"), FL(26, "GLASS"), FL(27, "MAP_RIVER"), FL(28, "SMOKE"),
    FL(29, "UNSMASHED"), FL(30, "MAP_STAIRS"), FL(31, "MAP_DEEP_SURFACE")};
#undef FL

template <size_t N> std::string flags_of(uint32_t v, const FlagName (&t)[N], const char* zero) { return flags_string(v, t, N, zero); }

const char* const kUsageNames[] = {"UNKNOWN", "DEFAULT", "TERRAIN", "CLOUDDENSITY", "CLOUDNORMAL", "CABLE", "FENCE", "ENVEFF", "SCRIPT",
    "WATERFLOW", "WATERFOAM", "WATERFOG", "WATEROCEAN", "WATER", "FOAMOPACITY", "FOAM", "DIFFUSEMIPSHARPEN", "DIFFUSEDETAIL", "DIFFUSEDARK",
    "DIFFUSEALPHAOPAQUE", "DIFFUSE", "DETAIL", "NORMAL", "SPECULAR", "EMISSIVE", "TINTPALETTE", "SKIPPROCESSING", "DONOTOPTIMIZE", "TEST",
    "COUNT"};

std::string format_name(TextureFormat f) {
    switch ((uint32_t)f) {
        case 21: return "D3DFMT_A8R8G8B8";
        case 22: return "D3DFMT_X8R8G8B8";
        case 25: return "D3DFMT_A1R5G5B5";
        case 28: return "D3DFMT_A8";
        case 32: return "D3DFMT_A8B8G8R8";
        case 50: return "D3DFMT_L8";
        case 81: return "D3DFMT_L16";
        case 0x31545844: return "D3DFMT_DXT1";
        case 0x33545844: return "D3DFMT_DXT3";
        case 0x35545844: return "D3DFMT_DXT5";
        case 0x31495441: return "D3DFMT_ATI1";
        case 0x32495441: return "D3DFMT_ATI2";
        case 0x20374342: return "D3DFMT_BC7";
    }
    return std::to_string((uint32_t)f);
}

const char* const kSemanticNames[16] = {"Position", "BlendWeights", "BlendIndices", "Normal", "Colour0", "Colour1", "TexCoord0", "TexCoord1",
    "TexCoord2", "TexCoord3", "TexCoord4", "TexCoord5", "TexCoord6", "TexCoord7", "Tangent", "Binormal"};

// ---------------------------------------------------------------- text helpers

void append_float(std::string& out, float f) {
    if (std::isnan(f)) { out += "NaN"; return; }
    if (std::isinf(f)) { out += f < 0 ? "-Infinity" : "Infinity"; return; }
    if (f == 0) { out += std::signbit(f) ? "-0" : "0"; return; }
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, f, std::chars_format::scientific); // shortest round trip: d.ddde+XX
    std::string s(buf, res.ptr);
    bool neg = s[0] == '-';
    if (neg) s.erase(0, 1);
    size_t e = s.find('e');
    int exp10 = std::atoi(s.c_str() + e + 1);
    std::string digits;
    for (size_t i = 0; i < e; i++) if (s[i] != '.') digits.push_back(s[i]);
    int point = exp10 + 1; // value = 0.DIGITS * 10^point
    if (neg) out += '-';
    if (point > 9 || point < -3) { // .NET: scientific outside 1E-4 .. 1E+8 of digits
        out += digits[0];
        if (digits.size() > 1) { out += '.'; out.append(digits, 1, std::string::npos); }
        char eb[16];
        std::snprintf(eb, sizeof eb, "E%c%02d", exp10 < 0 ? '-' : '+', std::abs(exp10));
        out += eb;
    } else if (point <= 0) {
        out += "0.";
        out.append((size_t)-point, '0');
        out += digits;
    } else if ((int)digits.size() <= point) {
        out += digits;
        out.append((size_t)point - digits.size(), '0');
    } else {
        out.append(digits, 0, (size_t)point);
        out += '.';
        out.append(digits, (size_t)point, std::string::npos);
    }
}

std::string fstr(float f) { std::string s; append_float(s, f); return s; }

std::string xml_escape(const std::string& s) { // CodeWalker: XmlNode.InnerText -> InnerXml, then " -> &quot;
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\r': out += "&#xD;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string vec3_attrs(const Vec3& v) { return "x=\"" + fstr(v.x) + "\" y=\"" + fstr(v.y) + "\" z=\"" + fstr(v.z) + "\""; }
std::string vec4_attrs(const Vec4& v) { return "x=\"" + fstr(v.x) + "\" y=\"" + fstr(v.y) + "\" z=\"" + fstr(v.z) + "\" w=\"" + fstr(v.w) + "\""; }

struct Out {
    std::string s;
    const XmlOptions& opt;
    explicit Out(const XmlOptions& o) : opt(o) {}

    /// The text, with CodeWalker's Windows line ends unless the options say otherwise.
    std::string finish() {
        if (!opt.crlf) return std::move(s);
        std::string r;
        r.reserve(s.size() + s.size() / 20);
        for (char c : s) { if (c == '\n') r += '\r'; r += c; }
        return r;
    }
    void indent(int n) { s.append((size_t)n, ' '); }
    void open(int ind, const std::string& name, bool nl = true) { indent(ind); s += '<'; s += name; s += '>'; if (nl) s += '\n'; }
    void close(int ind, const std::string& name, bool nl = true) { indent(ind); s += "</"; s += name; s += '>'; if (nl) s += '\n'; }
    void value(int ind, const char* name, const std::string& v) { indent(ind); s += '<'; s += name; s += " value=\""; s += v; s += "\" />\n"; }
    void value(int ind, const char* name, uint32_t v) { value(ind, name, std::to_string(v)); }
    void value_f(int ind, const char* name, float v) { value(ind, name, fstr(v)); }
    void one_line(int ind, const char* name, const std::string& text) { indent(ind); s += '<'; s += name; s += '>'; s += text; s += "</"; s += name; s += ">\n"; }
    void self_closing(int ind, const std::string& v) { indent(ind); s += '<'; s += v; s += " />\n"; }
    void string_tag(int ind, const char* name, const std::string& text) { if (!text.empty()) one_line(ind, name, text); else self_closing(ind, name); }

    /// CodeWalker's MetaXmlBase.HashString for a MetaHash.
    std::string hash_string(uint32_t h) const {
        if (h == 0) return "";
        if (auto n = lookup(kShaderNames, std::size(kShaderNames), h)) return n;
        if (opt.hash_name) { auto n = opt.hash_name(h); if (!n.empty()) return n; }
        return hash_name(h);
    }
};

// ---------------------------------------------------------------- textures

void write_texture_item(Out& o, const Texture& t, int ind) {
    uint8_t usage = (uint8_t)(t.usage_data & 0x1F);
    o.string_tag(ind, "Name", xml_escape(t.name));
    o.value(ind, "Unk32", t.unk32);
    o.string_tag(ind, "Usage", usage < std::size(kUsageNames) ? std::string(kUsageNames[usage]) : std::to_string(usage));
    o.string_tag(ind, "UsageFlags", flags_of(t.usage_data >> 5, kTextureUsageFlags, nullptr));
    o.value(ind, "ExtraFlags", t.extra_flags);
    o.value(ind, "Width", t.width);
    o.value(ind, "Height", t.height);
    o.value(ind, "MipLevels", t.levels);
    o.string_tag(ind, "Format", format_name(t.format));
    o.string_tag(ind, "FileName", xml_escape((t.name.empty() ? std::string("null") : t.name) + ".dds"));
}

void write_texture_dictionary(Out& o, const TextureDictionary& td, int ind) {
    if (td.textures.empty()) { o.self_closing(ind, "TextureDictionary"); return; }
    o.open(ind, "TextureDictionary");
    for (auto& t : td.textures) {
        o.open(ind + 1, "Item");
        write_texture_item(o, t, ind + 2);
        o.close(ind + 1, "Item");
    }
    o.close(ind, "TextureDictionary");
}

// ---------------------------------------------------------------- shaders

void write_shader_param(Out& o, const ShaderParam& p, int ind) {
    const char* pn = lookup(kParamNames, std::size(kParamNames), p.name_hash);
    std::string name = pn ? std::string(pn) : o.opt.unknown_param_as_number ? std::to_string(p.name_hash) : hash_name(p.name_hash);
    const char* ts = p.type == ParamType::Texture ? "Texture" : p.type == ParamType::Vector ? "Vector" : "Array";
    std::string head = "Item name=\"" + name + "\" type=\"" + ts + "\"";
    if (p.type == ParamType::Texture) {
        if (p.texture.empty()) { o.self_closing(ind, head); return; }
        o.open(ind, head);
        o.string_tag(ind + 1, "Name", xml_escape(p.texture));
        o.close(ind, "Item");
    } else if (p.type == ParamType::Vector) {
        if (p.values.empty()) o.self_closing(ind, head);
        else o.self_closing(ind, head + " " + vec4_attrs(p.values[0]));
    } else {
        if (p.values.empty()) { o.self_closing(ind, head); return; }
        o.open(ind, head);
        for (auto& v : p.values) o.self_closing(ind + 1, "Value " + vec4_attrs(v));
        o.close(ind, "Item");
    }
}

void write_shader(Out& o, const Shader& s, int ind) {
    o.string_tag(ind, "Name", o.hash_string(s.name_hash));
    o.string_tag(ind, "FileName", o.hash_string(s.sps_hash));
    o.value(ind, "RenderBucket", s.render_bucket);
    o.open(ind, "Parameters");
    for (auto& p : s.params) write_shader_param(o, p, ind + 1);
    o.close(ind, "Parameters");
}

void write_shader_group(Out& o, const ShaderGroup& sg, int ind) {
    if (sg.textures) write_texture_dictionary(o, *sg.textures, ind);
    if (sg.shaders.empty()) { o.self_closing(ind, "Shaders"); return; }
    o.open(ind, "Shaders");
    for (auto& s : sg.shaders) {
        o.open(ind + 1, "Item");
        write_shader(o, s, ind + 2);
        o.close(ind + 1, "Item");
    }
    o.close(ind, "Shaders");
}

// ---------------------------------------------------------------- skeleton, joints

void write_skeleton(Out& o, const Skeleton& sk, int ind) {
    o.value(ind, "Unknown1C", sk.unk1C);
    o.value(ind, "Unknown50", sk.unk50);
    o.value(ind, "Unknown54", sk.unk54);
    o.value(ind, "Unknown58", sk.unk58);
    if (sk.bones.empty()) { o.self_closing(ind, "Bones"); return; }
    o.open(ind, "Bones");
    for (auto& b : sk.bones) {
        o.open(ind + 1, "Item");
        int c = ind + 2;
        o.string_tag(c, "Name", b.name);
        o.value(c, "Tag", b.tag);
        o.value(c, "Index", std::to_string(b.index));
        o.value(c, "ParentIndex", std::to_string(b.parent));
        o.value(c, "SiblingIndex", std::to_string(b.next_sibling));
        o.string_tag(c, "Flags", flags_of(b.flags, kBoneFlags, "None"));
        o.self_closing(c, "Translation " + vec3_attrs(b.translation));
        o.self_closing(c, "Rotation " + vec4_attrs({b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w}));
        o.self_closing(c, "Scale " + vec3_attrs(b.scale));
        o.self_closing(c, "TransformUnk " + vec4_attrs(b.transform_unk));
        o.close(ind + 1, "Item");
    }
    o.close(ind, "Bones");
}

template <class T> T rd(const Bytes& b, size_t off) { T v; std::memcpy(&v, b.data() + off, sizeof(T)); return v; }
Vec3 rd3(const Bytes& b, size_t off) { return {rd<float>(b, off), rd<float>(b, off + 4), rd<float>(b, off + 8)}; }

void write_joints(Out& o, const Joints& j, int ind) {
    size_t nr = j.rotation_limits.size() / 0xC0, nt = j.translation_limits.size() / 0x40;
    if (!j.rotation_limits.empty()) {
        o.open(ind, "RotationLimits");
        for (size_t i = 0; i < nr; i++) {
            Bytes b(j.rotation_limits.begin() + i * 0xC0, j.rotation_limits.begin() + (i + 1) * 0xC0);
            o.open(ind + 1, "Item");
            o.value(ind + 2, "BoneId", rd<uint16_t>(b, 8));
            o.value(ind + 2, "UnknownA", rd<uint16_t>(b, 10));
            o.self_closing(ind + 2, "Min " + vec3_attrs(rd3(b, 0x5C)));
            o.self_closing(ind + 2, "Max " + vec3_attrs(rd3(b, 0x68)));
            o.close(ind + 1, "Item");
        }
        o.close(ind, "RotationLimits");
    }
    if (!j.translation_limits.empty()) {
        o.open(ind, "TranslationLimits");
        for (size_t i = 0; i < nt; i++) {
            Bytes b(j.translation_limits.begin() + i * 0x40, j.translation_limits.begin() + (i + 1) * 0x40);
            o.open(ind + 1, "Item");
            o.value(ind + 2, "BoneId", rd<uint32_t>(b, 8));
            o.self_closing(ind + 2, "Min " + vec3_attrs(rd3(b, 0x20)));
            o.self_closing(ind + 2, "Max " + vec3_attrs(rd3(b, 0x30)));
            o.close(ind + 1, "Item");
        }
        o.close(ind, "TranslationLimits");
    }
}

// ---------------------------------------------------------------- vertices, models

std::string colour_string(const uint8_t* p) { // R G B A in memory order (SharpDX Color(uint rgba))
    return std::to_string(p[0]) + " " + std::to_string(p[1]) + " " + std::to_string(p[2]) + " " + std::to_string(p[3]);
}

/// CodeWalker's VertexData.GetString(v, k, " "): "" for component types it cannot print.
void vertex_component(std::string& row, const uint8_t* p, VertexComponentType t) {
    auto f32 = [&](int i) { float v; std::memcpy(&v, p + 4 * i, 4); append_float(row, v); };
    auto h16 = [&](int i) { uint16_t h; std::memcpy(&h, p + 2 * i, 2); append_float(row, half_to_float(h)); };
    switch (t) {
        case VertexComponentType::Float: f32(0); break;
        case VertexComponentType::Float2: f32(0); row += ' '; f32(1); break;
        case VertexComponentType::Float3: f32(0); row += ' '; f32(1); row += ' '; f32(2); break;
        case VertexComponentType::Float4: f32(0); row += ' '; f32(1); row += ' '; f32(2); row += ' '; f32(3); break;
        case VertexComponentType::RGBA8SNorm:
            for (int i = 0; i < 4; i++) { if (i) row += ' '; append_float(row, (float)(int8_t)p[i] / 127.0f); }
            break;
        case VertexComponentType::Half2: h16(0); row += ' '; h16(1); break;
        case VertexComponentType::Half4: for (int i = 0; i < 4; i++) { if (i) row += ' '; h16(i); } break;
        case VertexComponentType::Colour: case VertexComponentType::UByte4: row += colour_string(p); break;
        default: break;
    }
}

std::string layout_type_name(uint64_t types) {
    switch (types) {
        case 0x7755555555996996ull: return "GTAV1";
        case 0x030000000199A006ull: return "GTAV2";
        case 0x0300000001996006ull: return "GTAV3";
        case 0x7655555555996996ull: return "GTAV4";
    }
    return std::to_string(types);
}

void write_geometry(Out& o, const Geometry& g, int ind) {
    o.value(ind, "ShaderIndex", g.shader_index);
    o.self_closing(ind, "BoundingBoxMin " + vec4_attrs(g.bounds.min));
    o.self_closing(ind, "BoundingBoxMax " + vec4_attrs(g.bounds.max));
    if (!g.bone_ids.empty()) {
        std::string ids;
        for (auto id : g.bone_ids) { if (!ids.empty()) ids += ", "; ids += std::to_string(id); }
        o.string_tag(ind, "BoneIDs", ids);
    }
    o.open(ind, "VertexBuffer");
    int v1 = ind + 1;
    o.value(v1, "Flags", g.vb_flags);
    o.open(v1, "Layout type=\"" + layout_type_name(g.layout.types) + "\"");
    for (int k = 0; k < 16; k++) if ((g.layout.flags >> k) & 1) o.self_closing(v1 + 1, kSemanticNames[k]);
    o.close(v1, "Layout");
    const char* data_tag = g.vb_data2_only ? "Data2" : "Data"; // CodeWalker: Data1 (first pointer) is <Data>, a distinct second block <Data2>
    o.open(v1, data_tag);
    uint32_t stride = g.layout.stride();
    std::string row;
    for (uint32_t v = 0; v < g.vertex_count; v++) {
        row.clear();
        const uint8_t* base = g.vertices.data() + (size_t)v * stride;
        uint32_t off = 0;
        for (int k = 0; k < 16; k++) {
            if (!((g.layout.flags >> k) & 1)) continue;
            auto t = g.layout.type_of((VertexSemantic)k);
            if (!row.empty()) row += "   ";
            if ((size_t)(v + 1) * stride <= g.vertices.size()) vertex_component(row, base + off, t);
            off += vertex_component_size(t);
        }
        o.indent(v1 + 1);
        o.s += row;
        o.s += '\n';
    }
    o.close(v1, data_tag);
    o.close(ind, "VertexBuffer");

    o.open(ind, "IndexBuffer");
    int n = (int)g.indices.size();
    const int row_size = 24;
    int d = ind + 1;
    if (n == 0) o.self_closing(d, "Data");
    else if (n <= row_size) {
        o.open(d, "Data", false);
        for (int i = 0; i < n; i++) { if (i) o.s += ' '; o.s += std::to_string(g.indices[i]); }
        o.close(0, "Data");
    } else {
        o.open(d, "Data");
        for (int i = 0; i < n; i++) {
            int col = i % row_size;
            if (col == 0) o.indent(d + 1);
            else o.s += ' ';
            o.s += std::to_string(g.indices[i]);
            if (col == row_size - 1 || i == n - 1) o.s += '\n';
        }
        o.close(d, "Data");
    }
    o.close(ind, "IndexBuffer");
}

void write_model_list(Out& o, const std::vector<Model>& models, const char* tag, int ind) {
    if (models.empty()) return;
    o.open(ind, tag);
    for (auto& m : models) {
        o.open(ind + 1, "Item");
        int c = ind + 2;
        o.value(c, "RenderMask", m.render_mask);
        o.value(c, "Flags", m.flags);
        o.value(c, "HasSkin", (uint32_t)((m.skeleton_binding >> 8) & 0xFF));
        o.value(c, "BoneIndex", (uint32_t)((m.skeleton_binding >> 24) & 0xFF));
        o.value(c, "Unknown1", (uint32_t)(m.skeleton_binding & 0xFF));
        if (m.geometries.empty()) o.self_closing(c, "Geometries");
        else {
            o.open(c, "Geometries");
            for (auto& g : m.geometries) {
                o.open(c + 1, "Item");
                write_geometry(o, g, c + 2);
                o.close(c + 1, "Item");
            }
            o.close(c, "Geometries");
        }
        o.close(ind + 1, "Item");
    }
    o.close(ind, tag);
}

// ---------------------------------------------------------------- lights

void write_light(Out& o, const Light& l, int ind) {
    o.self_closing(ind, "Position " + vec3_attrs(l.position));
    o.self_closing(ind, "Colour r=\"" + std::to_string(l.colour[0]) + "\" g=\"" + std::to_string(l.colour[1]) + "\" b=\"" + std::to_string(l.colour[2]) + "\"");
    o.value(ind, "Flashiness", l.flashiness);
    o.value_f(ind, "Intensity", l.intensity);
    o.value(ind, "Flags", l.flags);
    o.value(ind, "BoneId", l.bone_id);
    uint8_t ty = (uint8_t)l.type;
    o.string_tag(ind, "Type", ty == 1 ? "Point" : ty == 2 ? "Spot" : ty == 4 ? "Capsule" : std::to_string(ty));
    o.value(ind, "GroupId", l.group_id);
    o.value(ind, "TimeFlags", l.time_flags);
    o.value_f(ind, "Falloff", l.falloff);
    o.value_f(ind, "FalloffExponent", l.falloff_exponent);
    o.self_closing(ind, "CullingPlaneNormal " + vec3_attrs(l.culling_plane_normal));
    o.value_f(ind, "CullingPlaneOffset", l.culling_plane_offset);
    o.value(ind, "Unknown45", l.unk45);
    o.value(ind, "Unknown46", l.unk46);
    o.value_f(ind, "VolumeIntensity", l.volume_intensity);
    o.value_f(ind, "VolumeSizeScale", l.volume_size_scale);
    o.self_closing(ind, "VolumeOuterColour r=\"" + std::to_string(l.volume_outer_colour[0]) + "\" g=\"" + std::to_string(l.volume_outer_colour[1]) +
                            "\" b=\"" + std::to_string(l.volume_outer_colour[2]) + "\"");
    o.value(ind, "LightHash", l.light_hash);
    o.value_f(ind, "VolumeOuterIntensity", l.volume_outer_intensity);
    o.value_f(ind, "CoronaSize", l.corona_size);
    o.value_f(ind, "VolumeOuterExponent", l.volume_outer_exponent);
    o.value(ind, "LightFadeDistance", l.light_fade);
    o.value(ind, "ShadowBlur", l.shadow_blur);
    o.value(ind, "ShadowFadeDistance", l.shadow_fade);
    o.value(ind, "SpecularFadeDistance", l.specular_fade);
    o.value(ind, "VolumetricFadeDistance", l.volumetric_fade);
    o.value_f(ind, "ShadowNearClip", l.shadow_near_clip);
    o.value_f(ind, "CoronaIntensity", l.corona_intensity);
    o.value_f(ind, "CoronaZBias", l.corona_z_bias);
    o.self_closing(ind, "Direction " + vec3_attrs(l.direction));
    o.self_closing(ind, "Tangent " + vec3_attrs(l.tangent));
    o.value_f(ind, "ConeInnerAngle", l.cone_inner);
    o.value_f(ind, "ConeOuterAngle", l.cone_outer);
    o.self_closing(ind, "Extent " + vec3_attrs(l.extent));
    o.string_tag(ind, "ProjectedTextureHash", o.hash_string(l.projected_texture_hash));
}

// ---------------------------------------------------------------- bounds

void write_bound(Out& o, const Bound* b, int ind, const char* tag, const BoundChild* as_child);

void write_bound_body(Out& o, const Bound& b, int ind, const BoundChild* child) {
    o.self_closing(ind, "BoxMin " + vec3_attrs(b.box_min));
    o.self_closing(ind, "BoxMax " + vec3_attrs(b.box_max));
    o.self_closing(ind, "BoxCenter " + vec3_attrs(b.box_center));
    o.self_closing(ind, "SphereCenter " + vec3_attrs(b.sphere_center));
    o.value_f(ind, "SphereRadius", b.sphere_radius);
    o.value_f(ind, "Margin", b.margin);
    o.value_f(ind, "Volume", b.volume);
    o.self_closing(ind, "Inertia " + vec3_attrs(b.inertia));
    o.value(ind, "MaterialIndex", b.material_index);
    o.value(ind, "MaterialColourIndex", b.material_colour_index);
    o.value(ind, "ProceduralID", b.procedural_id);
    o.value(ind, "RoomID", (uint32_t)b.room_id());
    o.value(ind, "PedDensity", (uint32_t)b.ped_density());
    o.value(ind, "UnkFlags", b.unk_flags);
    o.value(ind, "PolyFlags", b.poly_flags);
    o.value(ind, "UnkType", b.unk3C);
    if (child) {
        // Matrix.ToArray(): 16 floats, row by row: the three columns with w 0, then the translation with w 1; rows of 4
        float m[16];
        for (int c = 0; c < 4; c++) { m[c * 4] = child->transform.col[c].x; m[c * 4 + 1] = child->transform.col[c].y; m[c * 4 + 2] = child->transform.col[c].z; m[c * 4 + 3] = c == 3 ? 1.f : 0.f; }
        o.open(ind, "CompositeTransform");
        for (int r = 0; r < 4; r++) {
            o.indent(ind + 1);
            for (int c = 0; c < 4; c++) { if (c) o.s += ' '; append_float(o.s, m[r * 4 + c]); }
            o.s += '\n';
        }
        o.close(ind, "CompositeTransform");
        o.string_tag(ind, "CompositeFlags1", flags_of(child->flags1, kCompositeFlags, "NONE"));
        o.string_tag(ind, "CompositeFlags2", flags_of(child->flags2, kCompositeFlags, "NONE"));
    }
    if (b.type == BoundType::Geometry || b.type == BoundType::GeometryBVH) {
        if (!b.geometry) return;
        const auto& g = *b.geometry;
        o.self_closing(ind, "GeometryCenter " + vec3_attrs(g.center));
        o.value_f(ind, "UnkFloat1", g.unk9C);
        o.value_f(ind, "UnkFloat2", g.unkAC);
        if (g.materials.empty()) o.self_closing(ind, "Materials"); // a bound with no polygons: CodeWalker keeps an empty (non-null) array
        else {
            o.open(ind, "Materials");
            for (auto& m : g.materials) {
                o.open(ind + 1, "Item");
                int c = ind + 2;
                o.value(c, "Type", m.type());
                o.value(c, "ProceduralID", m.procedural_id());
                o.value(c, "RoomID", m.room_id());
                o.value(c, "PedDensity", m.ped_density());
                o.string_tag(c, "Flags", flags_of(m.flags(), kMaterialFlags, "NONE"));
                o.value(c, "MaterialColourIndex", m.colour_index());
                o.value(c, "Unk", (uint32_t)(m.data2 >> 16));
                o.close(ind + 1, "Item");
            }
            o.close(ind, "Materials");
        }
        auto colour_text = [](uint32_t c) {
            return std::to_string(c & 0xFF) + ", " + std::to_string((c >> 8) & 0xFF) + ", " + std::to_string((c >> 16) & 0xFF) + ", " + std::to_string(c >> 24);
        };
        auto colours = [&](const char* tag, const std::vector<uint32_t>& v) {
            if (v.empty()) return;
            if (v.size() == 1) { o.open(ind, tag, false); o.s += colour_text(v[0]); o.close(0, tag); return; } // WriteRawArray, row size 1
            o.open(ind, tag);
            for (uint32_t c : v) { o.indent(ind + 1); o.s += colour_text(c); o.s += '\n'; }
            o.close(ind, tag);
        };
        colours("MaterialColours", g.material_colours);
        if (!g.vertices.empty()) {
            if (g.vertices.size() <= 1) { // WriteRawArray with row size 1: a single item stays on the tag's line
                o.open(ind, "Vertices", false);
                o.s += fstr(g.vertices[0].x) + ", " + fstr(g.vertices[0].y) + ", " + fstr(g.vertices[0].z);
                o.close(0, "Vertices");
            } else {
                o.open(ind, "Vertices");
                for (auto& v : g.vertices) {
                    o.indent(ind + 1);
                    o.s += fstr(v.x) + ", " + fstr(v.y) + ", " + fstr(v.z) + "\n";
                }
                o.close(ind, "Vertices");
            }
        }
        colours("VertexColours", g.vertex_colours);
        if (!g.polygons.empty()) {
            o.open(ind, "Polygons");
            for (size_t i = 0; i < g.polygons.size(); i++) {
                const auto& p = g.polygons[i];
                std::string m = std::to_string(i < g.polygon_materials.size() ? g.polygon_materials[i] : 0);
                std::string s;
                switch (p.kind) {
                    case BoundPolygon::Triangle:
                        s = "Triangle m=\"" + m + "\" v1=\"" + std::to_string(p.vertex(0)) + "\" v2=\"" + std::to_string(p.vertex(1)) + "\" v3=\"" +
                            std::to_string(p.vertex(2)) + "\" f1=\"" + (p.vertex_flag(0) ? "1" : "0") + "\" f2=\"" + (p.vertex_flag(1) ? "1" : "0") +
                            "\" f3=\"" + (p.vertex_flag(2) ? "1" : "0") + "\"";
                        break;
                    case BoundPolygon::Sphere:
                        s = "Sphere m=\"" + m + "\" v=\"" + std::to_string(p.vertex(0)) + "\" radius=\"" + fstr(p.radius()) + "\"";
                        break;
                    case BoundPolygon::Capsule: case BoundPolygon::Cylinder:
                        s = std::string(p.kind == BoundPolygon::Capsule ? "Capsule" : "Cylinder") + " m=\"" + m + "\" v1=\"" + std::to_string(p.vertex(0)) +
                            "\" v2=\"" + std::to_string(p.vertex(1)) + "\" radius=\"" + fstr(p.radius()) + "\"";
                        break;
                    case BoundPolygon::Box:
                        s = "Box m=\"" + m + "\" v1=\"" + std::to_string((int16_t)p.vertex(0)) + "\" v2=\"" + std::to_string((int16_t)p.vertex(1)) +
                            "\" v3=\"" + std::to_string((int16_t)p.vertex(2)) + "\" v4=\"" + std::to_string((int16_t)p.vertex(3)) + "\"";
                        break;
                }
                o.self_closing(ind + 1, s);
            }
            o.close(ind, "Polygons");
        }
    } else if (b.type == BoundType::Composite) {
        if (b.children.empty()) { o.self_closing(ind, "Children"); return; }
        o.open(ind, "Children");
        for (auto& ch : b.children) write_bound(o, ch.bound.get(), ind + 1, "Item", &ch);
        o.close(ind, "Children");
    }
}

void write_bound(Out& o, const Bound* b, int ind, const char* tag, const BoundChild* as_child) {
    if (!b) { o.self_closing(ind, std::string(tag) + " type=\"None\""); return; }
    o.open(ind, std::string(tag) + " type=\"" + bound_type_name(b->type) + "\"");
    write_bound_body(o, *b, ind + 1, as_child);
    o.close(ind, tag);
}

// ---------------------------------------------------------------- drawable

void write_drawable_body(Out& o, const Drawable& d, int ind) {
    o.string_tag(ind, "Name", xml_escape(d.name));
    o.self_closing(ind, "BoundingSphereCenter " + vec3_attrs(d.bounding_center));
    o.value_f(ind, "BoundingSphereRadius", d.bounding_radius);
    o.self_closing(ind, "BoundingBoxMin " + vec3_attrs(d.bbox_min));
    o.self_closing(ind, "BoundingBoxMax " + vec3_attrs(d.bbox_max));
    o.value_f(ind, "LodDistHigh", d.lod_distance[0]);
    o.value_f(ind, "LodDistMed", d.lod_distance[1]);
    o.value_f(ind, "LodDistLow", d.lod_distance[2]);
    o.value_f(ind, "LodDistVlow", d.lod_distance[3]);
    o.value(ind, "FlagsHigh", d.lod_flags(Lod::High));
    o.value(ind, "FlagsMed", d.lod_flags(Lod::Medium));
    o.value(ind, "FlagsLow", d.lod_flags(Lod::Low));
    o.value(ind, "FlagsVlow", d.lod_flags(Lod::VeryLow));
    if (d.shader_group) {
        o.open(ind, "ShaderGroup");
        write_shader_group(o, *d.shader_group, ind + 1);
        o.close(ind, "ShaderGroup");
    }
    if (d.skeleton) {
        o.open(ind, "Skeleton");
        write_skeleton(o, *d.skeleton, ind + 1);
        o.close(ind, "Skeleton");
    }
    if (d.joints) {
        o.open(ind, "Joints");
        write_joints(o, *d.joints, ind + 1);
        o.close(ind, "Joints");
    }
    write_model_list(o, d.lods[0], "DrawableModelsHigh", ind);
    write_model_list(o, d.lods[1], "DrawableModelsMedium", ind);
    write_model_list(o, d.lods[2], "DrawableModelsLow", ind);
    write_model_list(o, d.lods[3], "DrawableModelsVeryLow", ind);
    write_model_list(o, d.lods_extra, "DrawableModelsX", ind);
    if (d.bound) write_bound(o, d.bound.get(), ind, "Bounds", nullptr);
    if (d.lights.empty()) o.self_closing(ind, "Lights");
    else {
        o.open(ind, "Lights");
        for (auto& l : d.lights) {
            o.open(ind + 1, "Item");
            write_light(o, l, ind + 2);
            o.close(ind + 1, "Item");
        }
        o.close(ind, "Lights");
    }
}

const char* kHeader = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";

// CodeWalker YddFile.Load: a drawable whose name does not hash to its key is renamed HashString(hash).
std::string ydd_item_name(const Out& o, const Drawable& d, uint32_t hash) {
    if (!d.name.empty() && joaat(d.name) == hash) return d.name; // JenkHash.GenHash(string) does not lowercase
    return o.hash_string(hash);
}

} // namespace

// ================================================================ public

std::string xml_float(float f) { return fstr(f); }

const char* known_shader_name(uint32_t hash) { return lookup(kShaderNames, std::size(kShaderNames), hash); }
const char* known_param_name(uint32_t hash) { return lookup(kParamNames, std::size(kParamNames), hash); }

std::string write_drawable_xml(const Drawable& d, const XmlOptions& opt) {
    Out o(opt);
    o.s += kHeader; o.s += '\n';
    o.open(0, "Drawable");
    write_drawable_body(o, d, 1);
    o.close(0, "Drawable");
    return o.finish();
}

std::string write_drawable_dictionary_xml(const DrawableDictionary& dd, const XmlOptions& opt) {
    Out o(opt);
    o.s += kHeader; o.s += '\n';
    o.open(0, "DrawableDictionary");
    for (size_t i = 0; i < dd.drawables.size(); i++) {
        Drawable d = dd.drawables[i]; // by value: the item's name is what CodeWalker derives from the key
        if (i < dd.hashes.size()) d.name = ydd_item_name(o, d, dd.hashes[i]);
        o.open(1, "Item");
        write_drawable_body(o, d, 2);
        o.close(1, "Item");
    }
    o.close(0, "DrawableDictionary");
    return o.finish();
}

std::string write_bounds_xml(const Bound& b, const XmlOptions& opt) {
    Out o(opt);
    o.s += kHeader; o.s += '\n';
    o.open(0, "BoundsFile");
    write_bound(o, &b, 1, "Bounds", nullptr);
    o.close(0, "BoundsFile");
    return o.finish();
}

std::string write_texture_dictionary_xml(const TextureDictionary& td, const std::string&) {
    XmlOptions opt;
    Out o(opt);
    o.s += kHeader; o.s += '\n';
    write_texture_dictionary(o, td, 0);
    return o.finish();
}

std::vector<std::pair<std::string, Bytes>> dds_files(const TextureDictionary& td) {
    std::vector<std::pair<std::string, Bytes>> out;
    for (auto& t : td.textures) {
        Bytes dds = write_dds(t);
        // CodeWalker's DDSIO header: depth 1 and DDSD_MIPMAPCOUNT always set (write_dds leaves both off for a single level)
        if (dds.size() >= 128) { dds[24] = 1; dds[10] |= 0x02; }
        out.emplace_back((t.name.empty() ? std::string("null") : t.name) + ".dds", std::move(dds));
    }
    return out;
}

std::vector<std::pair<std::string, Bytes>> dds_files(const Drawable& d) {
    if (d.shader_group && d.shader_group->textures) return dds_files(*d.shader_group->textures);
    return {};
}

std::vector<std::pair<std::string, Bytes>> dds_files(const DrawableDictionary& dd) {
    std::vector<std::pair<std::string, Bytes>> out;
    for (auto& d : dd.drawables) for (auto& f : dds_files(d)) out.push_back(std::move(f));
    return out;
}

} // namespace rk
