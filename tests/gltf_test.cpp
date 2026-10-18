// gltf_test: what write_glb makes of a drawable, read back from the .glb (no game files needed).
//   - a triangle facing its vertex normals in the game faces them in glTF too (counter-clockwise front, the axis change
//     is a rotation)
//   - the draw buckets' alpha modes: 1 (alpha) and 2 (decal) blend, 3 (cutout) is alpha tested
// Exit 0 = every check passed.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "ragekit/drawable.h"
#include "ragekit/gltf.h"

using namespace rk;

static int fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%-100s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

struct Glb { std::string json; Bytes bin; };

static Glb split(const Bytes& b) {
    Glb g;
    uint32_t jl, bl;
    std::memcpy(&jl, &b[12], 4);
    g.json.assign((const char*)&b[20], jl);
    std::memcpy(&bl, &b[20 + jl], 4);
    g.bin.assign(b.begin() + 28 + jl, b.begin() + 28 + jl + bl);
    return g;
}

// The byteOffset of every bufferView, in order.
static std::vector<size_t> view_offsets(const std::string& json) {
    std::vector<size_t> out;
    size_t p = json.find("\"bufferViews\":[");
    while ((p = json.find("\"byteOffset\":", p)) != std::string::npos) {
        p += 13;
        out.push_back(std::stoul(json.substr(p, 20)));
    }
    return out;
}

static Drawable triangle(uint8_t bucket) {
    Drawable d;
    Geometry g;
    g.layout = VertexLayout::standard({VertexSemantic::Position, VertexSemantic::Normal});
    g.resize_vertices(3);
    const Vec3 p[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};   // counter-clockwise seen from +z
    for (uint32_t i = 0; i < 3; i++) {
        g.set(i, VertexSemantic::Position, {p[i].x, p[i].y, p[i].z, 0});
        g.set(i, VertexSemantic::Normal, {0, 0, 1, 0});
    }
    g.indices = {0, 1, 2};
    Model m;
    m.geometries.push_back(g);
    d.lods[0].push_back(m);
    ShaderGroup sg;
    Shader s;
    s.name_hash = 0x12345678;
    s.render_bucket = bucket;
    sg.shaders.push_back(s);
    d.shader_group = sg;
    return d;
}

int main() {
    {
        const Glb g = split(write_glb(triangle(0)));
        const std::vector<size_t> views = view_offsets(g.json);   // positions, normals, indices
        check(views.size() == 3, "gltf: one triangle makes three buffer views");
        if (views.size() == 3) {
            float pos[9], nrm[3];
            uint16_t idx[3];
            std::memcpy(pos, &g.bin[views[0]], sizeof pos);
            std::memcpy(nrm, &g.bin[views[1]], sizeof nrm);
            std::memcpy(idx, &g.bin[views[2]], sizeof idx);
            auto at = [&](int i) { return Vec3{pos[idx[i] * 3], pos[idx[i] * 3 + 1], pos[idx[i] * 3 + 2]}; };
            const Vec3 a = at(0), b = at(1), c = at(2), e1 = b - a, e2 = c - a;
            const Vec3 x{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
            const float dot = x.x * nrm[0] + x.y * nrm[1] + x.z * nrm[2];
            check(nrm[1] > 0.99f, "gltf: the game's +z normal is glTF's +y");
            check(dot > 0, "gltf: the triangle winds counter-clockwise about its normal, as in the game");
        }
    }
    const char* want[4] = {nullptr, "\"alphaMode\":\"BLEND\"", "\"alphaMode\":\"BLEND\"", "\"alphaMode\":\"MASK\""};
    for (uint8_t bucket = 0; bucket < 4; bucket++) {
        const Glb g = split(write_glb(triangle(bucket)));
        const bool ok = want[bucket] ? g.json.find(want[bucket]) != std::string::npos : g.json.find("alphaMode") == std::string::npos;
        check(ok, "gltf: draw bucket " + std::to_string(bucket) + " is " + (want[bucket] ? want[bucket] : "opaque"));
    }
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
