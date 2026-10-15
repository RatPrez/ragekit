// game_lookup: finds an archetype in a GTA V install by name and pulls its model and texture dictionary out of the archives.
//   game_lookup <archetype name> <out dir> [game dir]
// The first run scans the install (a few seconds) and caches the index; retail archives need the keys, see docs/archives.md.
#include <cstdio>
#include <filesystem>
#include <ragekit/ragekit.h>

using namespace rk;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: game_lookup <archetype name> <out dir> [game dir]\n");
        return 2;
    }
    try {
        const GameIndex gi = GameIndex::open(argc > 3 ? argv[3] : GameIndex::default_game_dir());
        const auto a = gi.archetype(argv[1]);
        if (!a) {
            std::fprintf(stderr, "no archetype named %s; close matches:\n", argv[1]);
            for (const IndexedArchetype& m : gi.find_archetypes(argv[1], 10)) std::fprintf(stderr, "  %s\n", gi.name(m.name).c_str());
            return 1;
        }
        const std::string out = argv[2];
        std::filesystem::create_directories(out);
        std::printf("%s: lodDist %.0f, box (%.1f %.1f %.1f) .. (%.1f %.1f %.1f)\n", argv[1], a->lod_dist, a->bb_min.x, a->bb_min.y, a->bb_min.z,
                    a->bb_max.x, a->bb_max.y, a->bb_max.z);
        const ResolvedDrawable model = gi.resolve_drawable(*a);
        if (model.file) {
            rsc::write_file(out + "/" + gi.file_name(*model.file), gi.extract(*model.file));
            std::printf("  model   %s\n", gi.file_name(*model.file).c_str());
        }
        if (const auto txd = gi.resolve_txd(a->texture_dictionary)) {
            rsc::write_file(out + "/" + gi.file_name(*txd), gi.extract(*txd));
            std::printf("  txd     %s\n", gi.file_name(*txd).c_str());
        }
    } catch (const Error& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
