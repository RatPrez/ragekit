#include "ragekit/gltf.h"
#include "ragekit/image.h"
#include "ragekit/hash.h"
#include "ragekit/error.h"
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>

namespace rk {

namespace {

// Game (X east, Y north, Z up) -> glTF (Y up, -Z forward): x' = x, y' = z, z' = -y
inline Vec3 to_gltf(const Vec3& v) { return {v.x, v.z, -v.y}; }

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            default: if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, 8, "\\u%04x", c); o += b; } else o += c;
        }
    }
    return o + "\"";
}

std::string fnum(float f) {
    if (!std::isfinite(f)) f = 0;
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", f);
    return b;
}

struct Glb {
    Bytes bin;
    std::vector<std::string> buffer_views, accessors, meshes, materials, textures, images, samplers, nodes;
    std::map<std::string, int> image_index; // texture name -> images[] index
    std::map<std::string, int> material_index;

    void align4() { while (bin.size() % 4) bin.push_back(0); }

    int view(const void* data, size_t n, int target = 0, int stride = 0) {
        align4();
        size_t off = bin.size();
        bin.insert(bin.end(), (const uint8_t*)data, (const uint8_t*)data + n);
        std::ostringstream s;
        s << "{\"buffer\":0,\"byteOffset\":" << off << ",\"byteLength\":" << n;
        if (target) s << ",\"target\":" << target;
        if (stride) s << ",\"byteStride\":" << stride;
        s << "}";
        buffer_views.push_back(s.str());
        return (int)buffer_views.size() - 1;
    }
    int accessor(int v, int component_type, size_t count, const char* type, bool normalized = false, const float* mn = nullptr, const float* mx = nullptr, int ncomp = 0) {
        std::ostringstream s;
        s << "{\"bufferView\":" << v << ",\"componentType\":" << component_type << ",\"count\":" << count << ",\"type\":\"" << type << "\"";
        if (normalized) s << ",\"normalized\":true";
        if (mn && mx) {
            s << ",\"min\":[";
            for (int i = 0; i < ncomp; i++) s << (i ? "," : "") << fnum(mn[i]);
            s << "],\"max\":[";
            for (int i = 0; i < ncomp; i++) s << (i ? "," : "") << fnum(mx[i]);
            s << "]";
        }
        s << "}";
        accessors.push_back(s.str());
        return (int)accessors.size() - 1;
    }
};

int add_image(Glb& g, const Texture& t) {
    auto it = g.image_index.find(t.name);
    if (it != g.image_index.end()) return it->second;
    Bytes png;
    try { png = write_png(decode_texture(t)); } catch (...) { return -1; }
    int v = g.view(png.data(), png.size());
    g.images.push_back("{\"bufferView\":" + std::to_string(v) + ",\"mimeType\":\"image/png\",\"name\":" + json_str(t.name) + "}");
    int idx = (int)g.images.size() - 1;
    g.image_index[t.name] = idx;
    return idx;
}

int add_texture(Glb& g, const Texture* t) {
    if (!t) return -1;
    int img = add_image(g, *t);
    if (img < 0) return -1;
    if (g.samplers.empty()) g.samplers.push_back("{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":10497,\"wrapT\":10497}");
    g.textures.push_back("{\"sampler\":0,\"source\":" + std::to_string(img) + "}");
    return (int)g.textures.size() - 1;
}

const Texture* lookup(const Drawable& d, const std::string& name, const GltfOptions& opt) {
    if (name.empty()) return nullptr;
    if (d.shader_group && d.shader_group->textures) {
        if (auto* t = d.shader_group->textures->find(name)) return t;
    }
    if (opt.find_texture) return opt.find_texture(name);
    return nullptr;
}

int add_material(Glb& g, const Drawable& d, const Shader& s, size_t index, const GltfOptions& opt) {
    std::string diffuse = s.texture_of("DiffuseSampler");
    std::string bump = s.texture_of("BumpSampler");
    std::string key = hash_name(s.name_hash) + "|" + diffuse + "|" + bump + "|" + std::to_string(s.render_bucket);
    auto it = g.material_index.find(key);
    if (it != g.material_index.end()) return it->second;
    std::ostringstream m;
    m << "{\"name\":" << json_str(hash_name(s.name_hash) + "_" + std::to_string(index)) << ",\"pbrMetallicRoughness\":{";
    m << "\"metallicFactor\":0,\"roughnessFactor\":0.8";
    int dt = opt.textures ? add_texture(g, lookup(d, diffuse, opt)) : -1;
    if (dt >= 0) m << ",\"baseColorTexture\":{\"index\":" << dt << "}";
    else m << ",\"baseColorFactor\":[0.8,0.8,0.8,1]";
    m << "}";
    int nt = opt.textures ? add_texture(g, lookup(d, bump, opt)) : -1;
    if (nt >= 0) m << ",\"normalTexture\":{\"index\":" << nt << "}";
    if (s.render_bucket == 1 || s.render_bucket == 3) m << ",\"alphaMode\":\"BLEND\"";
    else if (s.render_bucket == 2) m << ",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5";
    m << ",\"doubleSided\":false}";
    g.materials.push_back(m.str());
    int idx = (int)g.materials.size() - 1;
    g.material_index[key] = idx;
    return idx;
}

int add_mesh(Glb& g, const Drawable& d, const Model& model, const std::string& name, const GltfOptions& opt) {
    std::ostringstream prims;
    bool first = true;
    for (auto& geom : model.geometries) {
        if (geom.vertex_count == 0 || geom.indices.empty()) continue;
        uint32_t n = geom.vertex_count;
        std::vector<float> pos(n * 3), nrm, uv0, uv1, tan;
        std::vector<uint8_t> col;
        float mn[3] = {INFINITY, INFINITY, INFINITY}, mx[3] = {-INFINITY, -INFINITY, -INFINITY};
        bool has_n = geom.layout.has(VertexSemantic::Normal), has_uv0 = geom.layout.has(VertexSemantic::TexCoord0);
        bool has_uv1 = geom.layout.has(VertexSemantic::TexCoord1), has_t = geom.layout.has(VertexSemantic::Tangent);
        bool has_c = geom.layout.has(VertexSemantic::Colour0);
        if (has_n) nrm.resize(n * 3);
        if (has_uv0) uv0.resize(n * 2);
        if (has_uv1) uv1.resize(n * 2);
        if (has_t) tan.resize(n * 4);
        if (has_c) col.resize(n * 4);
        for (uint32_t i = 0; i < n; i++) {
            Vec3 p = to_gltf(geom.position(i));
            pos[i * 3] = p.x; pos[i * 3 + 1] = p.y; pos[i * 3 + 2] = p.z;
            mn[0] = std::fmin(mn[0], p.x); mn[1] = std::fmin(mn[1], p.y); mn[2] = std::fmin(mn[2], p.z);
            mx[0] = std::fmax(mx[0], p.x); mx[1] = std::fmax(mx[1], p.y); mx[2] = std::fmax(mx[2], p.z);
            if (has_n) {
                Vec3 nn = geom.normal(i);
                float l = nn.length();
                nn = l > 0 ? to_gltf(nn * (1 / l)) : Vec3{0, 1, 0};
                nrm[i * 3] = nn.x; nrm[i * 3 + 1] = nn.y; nrm[i * 3 + 2] = nn.z;
            }
            if (has_uv0) { Vec2 t = geom.uv(i, 0); uv0[i * 2] = t.x; uv0[i * 2 + 1] = t.y; }
            if (has_uv1) { Vec2 t = geom.uv(i, 1); uv1[i * 2] = t.x; uv1[i * 2 + 1] = t.y; }
            if (has_t) {
                Vec4 t4 = geom.get(i, VertexSemantic::Tangent);
                Vec3 t3 = t4.xyz();
                float l = t3.length();
                t3 = l > 0 ? to_gltf(t3 * (1 / l)) : Vec3{1, 0, 0};
                tan[i * 4] = t3.x; tan[i * 4 + 1] = t3.y; tan[i * 4 + 2] = t3.z; tan[i * 4 + 3] = t4.w >= 0 ? 1.f : -1.f;
            }
            if (has_c) {
                Vec4 c = geom.colour(i, 0);
                col[i * 4] = (uint8_t)std::lround(c.x * 255); col[i * 4 + 1] = (uint8_t)std::lround(c.y * 255);
                col[i * 4 + 2] = (uint8_t)std::lround(c.z * 255); col[i * 4 + 3] = (uint8_t)std::lround(c.w * 255);
            }
        }
        // glTF is counter-clockwise front; the axis change flips handedness, so swap winding
        std::vector<uint16_t> idx(geom.indices);
        for (size_t i = 0; i + 2 < idx.size(); i += 3) std::swap(idx[i + 1], idx[i + 2]);

        int a_pos = g.accessor(g.view(pos.data(), pos.size() * 4, 34962), 5126, n, "VEC3", false, mn, mx, 3);
        std::ostringstream p;
        p << (first ? "" : ",") << "{\"attributes\":{\"POSITION\":" << a_pos;
        if (has_n) p << ",\"NORMAL\":" << g.accessor(g.view(nrm.data(), nrm.size() * 4, 34962), 5126, n, "VEC3");
        if (has_uv0) p << ",\"TEXCOORD_0\":" << g.accessor(g.view(uv0.data(), uv0.size() * 4, 34962), 5126, n, "VEC2");
        if (has_uv1) p << ",\"TEXCOORD_1\":" << g.accessor(g.view(uv1.data(), uv1.size() * 4, 34962), 5126, n, "VEC2");
        if (has_t) p << ",\"TANGENT\":" << g.accessor(g.view(tan.data(), tan.size() * 4, 34962), 5126, n, "VEC4");
        if (has_c) p << ",\"COLOR_0\":" << g.accessor(g.view(col.data(), col.size(), 34962), 5121, n, "VEC4", true);
        p << "},\"indices\":" << g.accessor(g.view(idx.data(), idx.size() * 2, 34963), 5123, idx.size(), "SCALAR");
        if (d.shader_group && geom.shader_index < d.shader_group->shaders.size())
            p << ",\"material\":" << add_material(g, d, d.shader_group->shaders[geom.shader_index], geom.shader_index, opt);
        p << ",\"mode\":4}";
        prims << p.str();
        first = false;
    }
    if (first) return -1;
    g.meshes.push_back("{\"name\":" + json_str(name) + ",\"primitives\":[" + prims.str() + "]}");
    return (int)g.meshes.size() - 1;
}

std::string matrix_json(const Mat4& game) {
    // game row-vector matrix (translation in row 4) -> glTF column-major 4x4 with the axis change
    // v' = C * v where C maps game axes to glTF: rows (1,0,0),(0,0,1),(0,-1,0)
    // glTF node matrix M' = C * M^T * C^-1 (M^T because glTF uses column vectors)
    float C[3][3] = {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
    float R[3][3]; // M^T rotation part
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = game.at(j, i);
    float T[3][3], Mp[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { T[i][j] = 0; for (int k = 0; k < 3; k++) T[i][j] += C[i][k] * R[k][j]; }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { Mp[i][j] = 0; for (int k = 0; k < 3; k++) Mp[i][j] += T[i][k] * C[j][k]; } // C^-1 = C^T
    Vec3 t = to_gltf({game.at(3, 0), game.at(3, 1), game.at(3, 2)});
    // column-major output
    float out[16] = {Mp[0][0], Mp[1][0], Mp[2][0], 0, Mp[0][1], Mp[1][1], Mp[2][1], 0, Mp[0][2], Mp[1][2], Mp[2][2], 0, t.x, t.y, t.z, 1};
    std::string s = "[";
    for (int i = 0; i < 16; i++) s += (i ? "," : "") + fnum(out[i]);
    return s + "]";
}

} // namespace

Bytes write_glb(const std::vector<GltfInstance>& instances, const GltfOptions& opt) {
    Glb g;
    std::vector<int> scene_nodes;
    for (auto& inst : instances) {
        if (!inst.drawable) continue;
        const Drawable& d = *inst.drawable;
        std::vector<int> children;
        auto add_lod = [&](Lod lod, const std::string& suffix) {
            const auto& models = d.models(lod);
            for (size_t mi = 0; mi < models.size(); mi++) {
                std::string mname = inst.name + suffix + (models.size() > 1 ? "_" + std::to_string(mi) : "");
                int mesh = add_mesh(g, d, models[mi], mname, opt);
                if (mesh < 0) continue;
                g.nodes.push_back("{\"name\":" + json_str(mname) + ",\"mesh\":" + std::to_string(mesh) + "}");
                children.push_back((int)g.nodes.size() - 1);
            }
        };
        if (opt.all_lods_as_nodes) {
            static const char* names[] = {"_high", "_med", "_low", "_vlow"};
            for (int l = 0; l < 4; l++) add_lod((Lod)l, names[l]);
        } else {
            Lod lod = opt.lod;
            // fall back to the first LOD that has models
            for (int l = (int)lod; l < 4 && d.models((Lod)l).empty(); l++) lod = (Lod)(l + 1 < 4 ? l + 1 : l);
            add_lod(lod, "");
        }
        std::string node = "{\"name\":" + json_str(inst.name) + ",\"matrix\":" + matrix_json(inst.transform);
        if (!children.empty()) {
            node += ",\"children\":[";
            for (size_t i = 0; i < children.size(); i++) node += (i ? "," : "") + std::to_string(children[i]);
            node += "]";
        }
        node += "}";
        g.nodes.push_back(node);
        scene_nodes.push_back((int)g.nodes.size() - 1);
    }
    g.align4();

    auto join = [](const std::vector<std::string>& v) { std::string s; for (size_t i = 0; i < v.size(); i++) s += (i ? "," : "") + v[i]; return s; };
    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"ragekit\"},\"scene\":0,\"scenes\":[{\"nodes\":[";
    for (size_t i = 0; i < scene_nodes.size(); i++) json += (i ? "," : "") + std::to_string(scene_nodes[i]);
    json += "]}],\"nodes\":[" + join(g.nodes) + "],\"meshes\":[" + join(g.meshes) + "]";
    if (!g.materials.empty()) json += ",\"materials\":[" + join(g.materials) + "]";
    if (!g.textures.empty()) json += ",\"textures\":[" + join(g.textures) + "],\"images\":[" + join(g.images) + "],\"samplers\":[" + join(g.samplers) + "]";
    json += ",\"accessors\":[" + join(g.accessors) + "],\"bufferViews\":[" + join(g.buffer_views) + "]";
    json += ",\"buffers\":[{\"byteLength\":" + std::to_string(g.bin.size()) + "}]}";
    while (json.size() % 4) json += ' ';

    Bytes out;
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i))); };
    put32(0x46546C67); // glTF
    put32(2);
    put32((uint32_t)(12 + 8 + json.size() + 8 + g.bin.size()));
    put32((uint32_t)json.size()); put32(0x4E4F534A); // JSON
    out.insert(out.end(), json.begin(), json.end());
    put32((uint32_t)g.bin.size()); put32(0x004E4942); // BIN
    out.insert(out.end(), g.bin.begin(), g.bin.end());
    return out;
}

Bytes write_glb(const Drawable& d, const GltfOptions& opt) {
    return write_glb(std::vector<GltfInstance>{{&d, Mat4::identity(), d.name.empty() ? "drawable" : d.name}}, opt);
}

Bytes write_glb(const Fragment& f, const GltfOptions& opt) {
    std::vector<GltfInstance> inst;
    // children have no shader group of their own: borrow the main drawable's textures
    GltfOptions o = opt;
    const Drawable* main = f.drawable ? &*f.drawable : nullptr;
    auto outer = opt.find_texture;
    o.find_texture = [main, outer](std::string_view name) -> const Texture* {
        if (main && main->shader_group && main->shader_group->textures)
            if (auto* t = main->shader_group->textures->find(name)) return t;
        return outer ? outer(name) : nullptr;
    };
    for (auto& p : f.visual_parts()) inst.push_back({p.drawable, p.transform, p.name});
    return write_glb(inst, o);
}

} // namespace rk
