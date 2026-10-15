#include "ragekit/manifest.h"
#include "ragekit/pso.h"
#include "ragekit/hash.h"
#include <cstring>

namespace rk {

uint32_t manifest_hash(const std::string& name) { return joaat_lower(name); }

void Manifest::add_ityp_dependency(const std::string& ityp, const std::vector<std::string>& deps, uint32_t flags) {
    ManifestDependencies d;
    d.name = manifest_hash(ityp); d.flags = flags;
    for (auto& x : deps) d.deps.push_back(manifest_hash(x));
    ityp_dependencies_2.push_back(std::move(d));
}
void Manifest::add_imap_dependency(const std::string& imap, const std::vector<std::string>& ityps, uint32_t flags) {
    ManifestDependencies d;
    d.name = manifest_hash(imap); d.flags = flags;
    for (auto& x : ityps) d.deps.push_back(manifest_hash(x));
    imap_dependencies_2.push_back(std::move(d));
}
void Manifest::add_interior(const std::string& name, const std::vector<std::string>& ybns) {
    ManifestInterior i;
    i.name = manifest_hash(name);
    for (auto& x : ybns) i.bounds.push_back(manifest_hash(x));
    interiors.push_back(std::move(i));
}

namespace {

// name hashes used by the manifest (MetaName values = joaat of the lower-cased name)
struct Names {
    uint32_t root = joaat("CPackFileMetaData");
    uint32_t map_data_group = joaat("CMapDataGroup");
    uint32_t hd_txd = joaat("CHDTxdAssetBinding");
    uint32_t imap_dep = joaat("CImapDependency");
    uint32_t imap_deps = joaat("CImapDependencies");
    uint32_t ityp_deps = joaat("CItypDependencies");
    uint32_t interiors = joaat("CInteriorBoundsFiles");
    uint32_t f_groups = joaat("MapDataGroups"), f_hd = joaat("HDTxdBindingArray"), f_imap = joaat("imapDependencies"),
             f_imap2 = joaat("imapDependencies_2"), f_ityp2 = joaat("itypDependencies_2"), f_int = joaat("Interiors");
};
const Names& N() { static const Names n; return n; }

// --- reading --------------------------------------------------------------------------------------------------------

bool field(const PsoView& v, const PsoObject& o, uint32_t name, PsoField& f) { return v.find(o, name, f) != nullptr; }

std::vector<uint32_t> hash_array(const PsoView& v, const PsoObject& o, uint32_t name) {
    std::vector<uint32_t> r;
    PsoField f;
    if (!field(v, o, name, f)) return r;
    PsoArrayRef a = v.get_array(f);
    if (!a.valid) return r;
    for (uint32_t i = 0; i < a.count; i++) {
        int64_t x = 0;
        if (!v.array_int(a, i, x)) throw Error("ymf: unreadable hash array");
        r.push_back((uint32_t)x);
    }
    return r;
}
uint32_t hash_field(const PsoView& v, const PsoObject& o, uint32_t name) {
    PsoField f;
    int64_t x = 0;
    if (field(v, o, name, f)) v.get_int(f, x);
    return (uint32_t)x;
}
template <class F> void each_item(const PsoView& v, const PsoObject& root, uint32_t name, F fn) {
    PsoField f;
    if (!field(v, root, name, f)) return;
    PsoArrayRef a = v.get_array(f);
    if (!a.valid) throw Error("ymf: unreadable array");
    for (uint32_t i = 0; i < a.count; i++) {
        PsoObject o = v.array_struct(a, i);
        if (!o) throw Error("ymf: array element without structure info");
        fn(o);
    }
}

// --- writing --------------------------------------------------------------------------------------------------------

struct Block {
    uint32_t name;
    Bytes data;
};
struct Ptr { uint32_t block = 0, offset = 0, count = 0; };

struct Builder {
    std::vector<Block> blocks;

    size_t ensure(uint32_t name) {
        for (size_t i = 0; i < blocks.size(); i++) if (blocks[i].name == name) return i;
        blocks.push_back({name, {}});
        return blocks.size() - 1;
    }
    Ptr add(uint32_t name, const Bytes& data, uint32_t count) {
        size_t b = ensure(name);
        Ptr p{uint32_t(b + 1), uint32_t(blocks[b].data.size()), count};
        blocks[b].data.insert(blocks[b].data.end(), data.begin(), data.end());
        return p;
    }
    Ptr add_hashes(const std::vector<uint32_t>& v) {
        if (v.empty()) return {};
        Bytes d;
        for (uint32_t h : v) put32(d, h);
        return add(6, d, (uint32_t)v.size()); // block name 6 = an array of unsigned ints
    }
    static void put32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
    static void put16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
    /// 16 byte array header: pointer word (block | offset << 12), unused word, count, capacity, unused.
    static void header(Bytes& b, const Ptr& p) {
        put32(b, p.block ? (p.block & 0xFFF) | ((p.offset & 0xFFFFF) << 12) : 0);
        put32(b, 0);
        put16(b, (uint16_t)p.count); put16(b, (uint16_t)p.count);
        put32(b, 0);
    }
};

PsoStructEntry entry(uint32_t name, uint8_t type, uint8_t sub, uint16_t off, uint32_t ref) {
    PsoStructEntry e; e.name = name; e.type = type; e.subtype = sub; e.offset = off; e.ref_key = ref;
    return e;
}
PsoElement structure(uint32_t name, int32_t size, std::vector<PsoStructEntry> entries) {
    PsoElement e; e.name = name; e.st.size = size; e.st.entries = std::move(entries);
    return e;
}
PsoElement enumeration(uint32_t name, std::vector<std::pair<const char*, int>> values) {
    PsoElement e; e.name = name; e.is_enum = true;
    for (auto& v : values) e.en.entries.push_back({joaat(v.first), v.second});
    return e;
}

} // namespace

Manifest read_ymf(std::span<const uint8_t> file) {
    if (!is_pso(file)) throw Error("ymf: not a PSO manifest (RBF and RSC7 manifests are not supported)");
    Pso p = read_pso(file);
    if (!p.has_map || !p.has_schema) throw Error("ymf: PSO without a readable PMAP / PSCH");
    PsoView v(p);
    PsoObject root = v.root();
    if (!root || root.type != N().root) throw Error("ymf: root structure is not CPackFileMetaData");
    Manifest m;
    each_item(v, root, N().f_groups, [&](const PsoObject& o) {
        ManifestMapDataGroup g;
        g.name = hash_field(v, o, joaat("Name"));
        g.bounds = hash_array(v, o, joaat("Bounds"));
        g.flags = hash_field(v, o, joaat("Flags"));
        g.weather_types = hash_array(v, o, joaat("WeatherTypes"));
        g.hours_on_off = hash_field(v, o, joaat("HoursOnOff"));
        m.map_data_groups.push_back(std::move(g));
    });
    each_item(v, root, N().f_hd, [&](const PsoObject& o) {
        ManifestHdTxd h;
        h.asset_type = (int32_t)hash_field(v, o, joaat("assetType"));
        PsoField f;
        if (field(v, o, joaat("targetAsset"), f)) v.get_string(f, h.target_asset);
        if (field(v, o, joaat("HDTxd"), f)) v.get_string(f, h.hd_txd);
        m.hd_txd_bindings.push_back(std::move(h));
    });
    each_item(v, root, N().f_imap, [&](const PsoObject& o) {
        ManifestImapDependency d;
        d.imap_name = hash_field(v, o, joaat("imapName"));
        d.ityp_name = hash_field(v, o, joaat("itypName"));
        d.pack_file_name = hash_field(v, o, joaat("packFileName"));
        m.imap_dependencies.push_back(d);
    });
    each_item(v, root, N().f_imap2, [&](const PsoObject& o) {
        ManifestDependencies d;
        d.name = hash_field(v, o, joaat("imapName"));
        d.flags = hash_field(v, o, joaat("manifestFlags"));
        d.deps = hash_array(v, o, joaat("itypDepArray"));
        m.imap_dependencies_2.push_back(std::move(d));
    });
    each_item(v, root, N().f_ityp2, [&](const PsoObject& o) {
        ManifestDependencies d;
        d.name = hash_field(v, o, joaat("itypName"));
        d.flags = hash_field(v, o, joaat("manifestFlags"));
        d.deps = hash_array(v, o, joaat("itypDepArray"));
        m.ityp_dependencies_2.push_back(std::move(d));
    });
    each_item(v, root, N().f_int, [&](const PsoObject& o) {
        ManifestInterior i;
        i.name = hash_field(v, o, joaat("Name"));
        i.bounds = hash_array(v, o, joaat("Bounds"));
        m.interiors.push_back(std::move(i));
    });
    return m;
}

Bytes write_ymf(const Manifest& m) {
    using namespace psotype;
    const Names& n = N();
    Builder b;
    Ptr groups, hds, imaps, imaps2, ityps2, ints;

    // each array: items first (their hash arrays are created while the item is built), then the item bytes in one go
    if (!m.map_data_groups.empty()) {
        Bytes all;
        for (auto& g : m.map_data_groups) {
            Ptr pb = b.add_hashes(g.bounds), pw = b.add_hashes(g.weather_types);
            Bytes d;
            Builder::put32(d, g.name); Builder::put32(d, 0);
            Builder::header(d, pb);
            Builder::put32(d, g.flags); Builder::put32(d, 0);       // +24 flags (int), +28 unused
            Builder::header(d, pw);
            Builder::put32(d, g.hours_on_off); Builder::put32(d, 0);
            all.insert(all.end(), d.begin(), d.end());
        }
        groups = b.add(n.map_data_group, all, (uint32_t)m.map_data_groups.size());
    }
    if (!m.hd_txd_bindings.empty()) {
        Bytes all;
        for (auto& h : m.hd_txd_bindings) {
            Bytes d(132, 0);
            for (int s = 0; s < 4; s++) d[s] = uint8_t(uint32_t(h.asset_type) >> (24 - 8 * s));
            std::memcpy(d.data() + 4, h.target_asset.data(), std::min<size_t>(h.target_asset.size(), 63));
            std::memcpy(d.data() + 68, h.hd_txd.data(), std::min<size_t>(h.hd_txd.size(), 63));
            all.insert(all.end(), d.begin(), d.end());
        }
        hds = b.add(n.hd_txd, all, (uint32_t)m.hd_txd_bindings.size());
    }
    if (!m.imap_dependencies.empty()) {
        Bytes all;
        for (auto& d : m.imap_dependencies) { Builder::put32(all, d.imap_name); Builder::put32(all, d.ityp_name); Builder::put32(all, d.pack_file_name); }
        imaps = b.add(n.imap_dep, all, (uint32_t)m.imap_dependencies.size());
    }
    auto deps_array = [&](const std::vector<ManifestDependencies>& v, uint32_t type) {
        Ptr r;
        if (v.empty()) return r;
        Bytes all;
        for (auto& d : v) {
            Ptr pd = b.add_hashes(d.deps);
            Builder::put32(all, d.name); Builder::put32(all, d.flags);
            Builder::header(all, pd);
        }
        return b.add(type, all, (uint32_t)v.size());
    };
    imaps2 = deps_array(m.imap_dependencies_2, n.imap_deps);
    ityps2 = deps_array(m.ityp_dependencies_2, n.ityp_deps);
    if (!m.interiors.empty()) {
        Bytes all;
        for (auto& i : m.interiors) {
            Ptr pb = b.add_hashes(i.bounds);
            Builder::put32(all, i.name); Builder::put32(all, 0);
            Builder::header(all, pb);
        }
        ints = b.add(n.interiors, all, (uint32_t)m.interiors.size());
    }
    Bytes rootb;
    for (const Ptr* p : {&groups, &hds, &imaps, &imaps2, &ityps2, &ints}) Builder::header(rootb, *p);
    b.add(n.root, rootb, 1);
    uint32_t root_id = (uint32_t)b.blocks.size(); // the root was added last

    // schema: only the types in use, children before the root, enums after the structures
    Pso p;
    uint32_t arrayinfo = kArrayInfo;
    bool flags_enum = !m.imap_dependencies_2.empty() || !m.ityp_dependencies_2.empty();
    uint32_t manifest_flags = joaat("manifestFlags");
    uint32_t group_flags = 1193003611u, asset_type = 3387532954u;
    auto& el = p.schema.elements;
    if (!m.map_data_groups.empty())
        el.push_back(structure(n.map_data_group, 56, {
            entry(joaat("Name"), String, 7, 0, 0),
            entry(arrayinfo, String, 7, 0, 0), entry(joaat("Bounds"), Array, 0, 8, 1),
            entry(arrayinfo, Enum, 0, 0, group_flags), entry(joaat("Flags"), Flags, 0, 24, 2097155),
            entry(arrayinfo, String, 7, 0, 0), entry(joaat("WeatherTypes"), Array, 0, 32, 5),
            entry(joaat("HoursOnOff"), UInt, 0, 48, 0)}));
    if (!m.hd_txd_bindings.empty())
        el.push_back(structure(n.hd_txd, 132, {
            entry(joaat("assetType"), Enum, 0, 0, asset_type),
            entry(joaat("targetAsset"), String, 0, 4, 4194304),
            entry(joaat("HDTxd"), String, 0, 68, 4194304)}));
    if (!m.imap_dependencies.empty())
        el.push_back(structure(n.imap_dep, 12, {
            entry(joaat("imapName"), String, 7, 0, 0), entry(joaat("itypName"), String, 7, 4, 0),
            entry(joaat("packFileName"), String, 7, 8, 0)}));
    auto dep_struct = [&](uint32_t type, const char* name_field) {
        return structure(type, 24, {
            entry(joaat(name_field), String, 7, 0, 0),
            entry(arrayinfo, Enum, 0, 0, manifest_flags), entry(manifest_flags, Flags, 0, 4, 2097153),
            entry(arrayinfo, String, 7, 0, 0), entry(joaat("itypDepArray"), Array, 0, 8, 3)});
    };
    if (!m.imap_dependencies_2.empty()) el.push_back(dep_struct(n.imap_deps, "imapName"));
    if (!m.ityp_dependencies_2.empty()) el.push_back(dep_struct(n.ityp_deps, "itypName"));
    if (!m.interiors.empty())
        el.push_back(structure(n.interiors, 24, {
            entry(joaat("Name"), String, 7, 0, 0),
            entry(arrayinfo, String, 7, 0, 0), entry(joaat("Bounds"), Array, 0, 8, 1)}));
    el.push_back(structure(n.root, 96, {
        entry(arrayinfo, Structure, 0, 0, n.map_data_group), entry(n.f_groups, Array, 0, 0, 0),
        entry(arrayinfo, Structure, 0, 0, n.hd_txd), entry(n.f_hd, Array, 0, 16, 2),
        entry(arrayinfo, Structure, 0, 0, n.imap_dep), entry(n.f_imap, Array, 0, 32, 4),
        entry(arrayinfo, Structure, 0, 0, n.imap_deps), entry(n.f_imap2, Array, 0, 48, 6),
        entry(arrayinfo, Structure, 0, 0, n.ityp_deps), entry(n.f_ityp2, Array, 0, 64, 8),
        entry(arrayinfo, Structure, 0, 0, n.interiors), entry(n.f_int, Array, 0, 80, 10)}));
    if (flags_enum) el.push_back(enumeration(manifest_flags, {{"INTERIOR_DATA", 0}}));
    if (!m.map_data_groups.empty()) el.push_back(enumeration(group_flags, {{"TIME_DEPENDENT", 0}, {"WEATHER_DEPENDENT", 1}}));
    if (!m.hd_txd_bindings.empty())
        el.push_back(enumeration(asset_type, {{"AT_TXD", 0}, {"AT_DRB", 1}, {"AT_DWD", 2}, {"AT_FRG", 3}}));

    // data + map
    PsoSection psin; psin.ident = kPsoPSIN;
    psin.payload.assign(8, 0); // the 16 reserved header bytes minus the 8 of the section header
    int32_t offset = 16;
    for (auto& blk : b.blocks) {
        PsoBlock pb; pb.name = blk.name; pb.offset = offset; pb.length = (int32_t)blk.data.size();
        p.map.blocks.push_back(pb);
        psin.payload.insert(psin.payload.end(), blk.data.begin(), blk.data.end());
        offset += (int32_t)blk.data.size();
    }
    p.map.root_id = (int32_t)root_id;
    p.has_map = p.has_schema = true;
    PsoSection pmap; pmap.ident = kPsoPMAP; pmap.parsed = true;
    PsoSection psch; psch.ident = kPsoPSCH; psch.parsed = true;
    p.sections = {psin, pmap, psch};
    return write_pso(p);
}

} // namespace rk
