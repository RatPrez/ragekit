// to_gltf: a drawable as a binary glTF (.glb) with its textures, for Blender and other 3D tools.
//   to_gltf <file.ydr> <out.glb> [textures.ytd ...]
// Textures the drawable does not embed are looked up in the .ytd files given.
#include <cstdio>
#include <ragekit/ragekit.h>

using namespace rk;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: to_gltf <file.ydr> <out.glb> [textures.ytd ...]\n");
        return 2;
    }
    try {
        const Drawable d = read_ydr(rsc::read_file(argv[1]));
        std::vector<TextureDictionary> txds;
        for (int i = 3; i < argc; i++) txds.push_back(read_ytd(rsc::read_file(argv[i])));

        GltfOptions opt;
        opt.find_texture = [&](std::string_view name) -> const Texture* {
            for (const TextureDictionary& t : txds)
                if (const Texture* found = t.find(name)) return found;
            return nullptr;
        };
        rsc::write_file(argv[2], write_glb(d, opt));   // Z-up game space becomes glTF's Y-up
        std::printf("wrote %s\n", argv[2]);
    } catch (const Error& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
