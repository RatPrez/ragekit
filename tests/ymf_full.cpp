// rk_ymf_full <out.ymf>: writes a manifest using every array (groups, HD txd, imap deps, dependencies, interiors)
// for checking against CodeWalker (oracle ymf --xml).
#include <cstdio>
#include "ragekit/ragekit.h"
using namespace rk;
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    Manifest m;
    ManifestMapDataGroup g; g.name = manifest_hash("grp_a"); g.bounds = {manifest_hash("b1"), manifest_hash("b2")}; g.flags = 3;
    g.weather_types = {manifest_hash("rain")}; g.hours_on_off = 0xFF00;
    m.map_data_groups.push_back(g);
    ManifestHdTxd h; h.asset_type = 2; h.target_asset = "prop_a"; h.hd_txd = "prop_a_hi";
    m.hd_txd_bindings.push_back(h);
    m.imap_dependencies.push_back({manifest_hash("imap1"), manifest_hash("ityp1"), manifest_hash("pack1")});
    m.add_imap_dependency("imap1", {"ityp1", "ityp2"});
    m.add_ityp_dependency("ityp1", {"ityp2"});
    m.add_ityp_dependency("ityp3");
    m.add_interior("int1", {"ybn1", "ybn2"});
    m.add_interior("int2", {});
    Bytes w = write_ymf(m);
    rsc::write_file(argv[1], w);
    Manifest r = read_ymf(w);
    bool ok = r.map_data_groups.size() == 1 && r.map_data_groups[0].bounds == g.bounds && r.map_data_groups[0].weather_types == g.weather_types &&
              r.map_data_groups[0].hours_on_off == g.hours_on_off && r.hd_txd_bindings.size() == 1 && r.hd_txd_bindings[0].hd_txd == "prop_a_hi" &&
              r.imap_dependencies.size() == 1 && r.imap_dependencies_2[0].deps.size() == 2 && r.ityp_dependencies_2.size() == 2 &&
              r.interiors.size() == 2 && r.interiors[0].bounds.size() == 2 && r.interiors[1].bounds.empty();
    std::printf("%s: %zu bytes, read back %s\n", argv[1], w.size(), ok ? "ok" : "MISMATCH");
    return ok ? 0 : 1;
}
