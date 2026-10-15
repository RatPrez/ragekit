// ydr_info: what a drawable holds - its LODs, models, geometries, shaders and embedded textures.
//   ydr_info <file.ydr>
#include <cstdio>
#include <ragekit/ragekit.h>

using namespace rk;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ydr_info <file.ydr>\n");
        return 2;
    }
    try {
        const Drawable d = read_ydr(rsc::read_file(argv[1]));
        std::printf("drawable '%s', box (%.2f %.2f %.2f) .. (%.2f %.2f %.2f)\n", d.name.c_str(), d.bbox_min.x, d.bbox_min.y, d.bbox_min.z,
                    d.bbox_max.x, d.bbox_max.y, d.bbox_max.z);

        static const char* lod_names[] = {"high", "medium", "low", "very low"};
        for (int l = 0; l < 4; l++) {
            const auto& models = d.models((Lod)l);
            if (models.empty()) continue;
            std::printf("%s LOD (up to %.0f m): %zu models\n", lod_names[l], d.lod_distance[(size_t)l], models.size());
            for (size_t m = 0; m < models.size(); m++)
                for (size_t k = 0; k < models[m].geometries.size(); k++) {
                    const Geometry& g = models[m].geometries[k];
                    std::printf("  model %zu geometry %zu: %u vertices, %zu triangles, shader %u\n", m, k, g.vertex_count, g.indices.size() / 3,
                                g.shader_index);
                }
        }

        if (d.shader_group) {
            for (size_t i = 0; i < d.shader_group->shaders.size(); i++) {
                const Shader& s = d.shader_group->shaders[i];
                std::printf("shader %zu: preset 0x%08X, diffuse '%s'\n", i, s.sps_hash, s.texture_of("DiffuseSampler").c_str());
            }
            if (d.shader_group->textures)
                for (const Texture& t : d.shader_group->textures->textures)
                    std::printf("embedded texture %s %ux%u %s\n", t.name.c_str(), t.width, t.height, texture_format_name(t.format));
        }
        if (d.bound) std::printf("embedded collision\n");
    } catch (const Error& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
