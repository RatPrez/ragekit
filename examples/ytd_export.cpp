// ytd_export: every texture of a texture dictionary as .dds (as stored) and .png (decoded).
//   ytd_export <file.ytd> <out dir>
#include <cstdio>
#include <filesystem>
#include <ragekit/ragekit.h>

using namespace rk;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ytd_export <file.ytd> <out dir>\n");
        return 2;
    }
    try {
        const std::string out = argv[2];
        std::filesystem::create_directories(out);
        const TextureDictionary txd = read_ytd(rsc::read_file(argv[1]));
        for (const Texture& t : txd.textures) {
            rsc::write_file(out + "/" + t.name + ".dds", write_dds(t));
            rsc::write_file(out + "/" + t.name + ".png", write_png(decode_texture(t)));
            std::printf("%-40s %4ux%-4u %-10s %u levels\n", t.name.c_str(), t.width, t.height, texture_format_name(t.format), t.levels);
        }
    } catch (const Error& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
