// rk_gameindex_test: the game index's archives against the install (skipped without one): archive(i) opens the archive
// the index lists, its entries are the files the index holds, and archive_rank gives each archive its place in the load
// order (base, update.rpf, a DLC pack and what is nested in them).
#include <algorithm>
#include <cstdio>
#include <string>
#include "ragekit/ragekit.h"

using namespace rk;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL  %s\n", what.c_str()); failures++; }
    else std::printf("ok    %s\n", what.c_str());
}

static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : GameIndex::default_game_dir();
    if (dir.empty()) { std::printf("no install: nothing checked\n"); return 0; }
    const GameIndex gi = GameIndex::open(dir);
    const auto& as = gi.archives();
    check(!as.empty(), std::to_string(as.size()) + " archives");

    // every 500th file: its archive opens and its entry has the file's name and size
    size_t n = 0, bad = 0;
    for (size_t i = 0; i < gi.files().size(); i += 500, n++) {
        const IndexedFile& f = gi.files()[i];
        const Rpf r = gi.archive(f.archive);
        const RpfEntry& e = r.entries().at(f.entry);
        if (lower(e.name) != gi.file_name(f) || e.size() != f.size) bad++;
    }
    check(bad == 0, std::to_string(n) + " files' entries found by archive(), " + std::to_string(bad) + " differ");

    // ranks: base archives tier 0, update.rpf 1, a pack's dlc.rpf and the archives nested in it the pack's
    size_t checked = 0;
    for (uint32_t i = 0; i < as.size(); i++) {
        const std::string p = lower(as[i].path);
        const LoadRank& r = gi.archive_rank(i);
        if (p == "update/update.rpf") check(r.tier == 1 && r.patch == 0, "update/update.rpf is tier 1 (" + r.label() + ")");
        else if (p == "x64a.rpf" || p == "common.rpf") check(r.tier == 0, p + " is tier 0 (" + r.label() + ")");
        else if (p == "update/x64/dlcpacks/mpheist/dlc.rpf") check(r.tier == 3 && r.dlc == "mpheist", p + " is the pack mpheist (" + r.label() + ")");
        else if (p.rfind("update/x64/dlcpacks/mpheist/dlc.rpf/", 0) == 0 && checked++ < 3)
            check(r.tier == 3 && r.dlc == "mpheist", p + " ranks with its pack (" + r.label() + ")");
    }
    // a nested archive opened twice is the same archive
    uint32_t nested = 0;
    while (nested < as.size() && as[nested].parent < 0) nested++;
    if (nested < as.size()) check(gi.archive(nested).entries().size() == gi.archive(nested).entries().size() && as[nested].file_count > 0, "a nested archive opens");
    bool threw = false;
    try { (void)gi.archive((uint32_t)as.size()); } catch (const Error&) { threw = true; }
    check(threw, "an index past the list throws");
    std::printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
