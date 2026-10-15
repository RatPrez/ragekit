// rpf_list: the files of an RPF7 archive, nested archives included, and optionally one file extracted.
//   rpf_list <file.rpf>                     list
//   rpf_list <file.rpf> <entry path> <out>  extract (the path may pass through nested archives: "x64/a.rpf/b.ydr")
// Archives encrypted with AES or NG (the retail ones) need the game's keys, see docs/archives.md.
#include <cstdio>
#include <ragekit/ragekit.h>

using namespace rk;

static void list(const Rpf& rpf, const std::string& prefix) {
    for (const RpfEntry& e : rpf.entries()) {
        if (e.is_directory()) continue;
        std::printf("%-9s %10llu  %s%s\n", e.is_resource() ? "resource" : "binary", (unsigned long long)e.size(), prefix.c_str(), e.path.c_str());
        if (e.is_archive()) list(rpf.open_child((int)(&e - rpf.entries().data())), prefix + e.path + "/");
    }
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        std::fprintf(stderr, "usage: rpf_list <file.rpf> [<entry path> <out file>]\n");
        return 2;
    }
    try {
        const Rpf rpf = Rpf::open(argv[1]);
        if (argc == 2) {
            list(rpf, "");
            return 0;
        }
        const Rpf::Location at = rpf.resolve(argv[2]);
        if (at.entry < 0) {
            std::fprintf(stderr, "%s is not in the archive\n", argv[2]);
            return 1;
        }
        rsc::write_file(argv[3], at.archive->extract(at.entry));   // a resource comes out as a complete RSC7 file
    } catch (const Error& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
