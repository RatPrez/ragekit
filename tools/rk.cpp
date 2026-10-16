// rk - ragekit command line tool.
//
//   rk info <file>                 header, segments, and a summary of the contents
//   rk dump <file>                 the full structure as text
//   rk roundtrip <in> <out>        read, write, read back, compare
//   rk ytd export <ytd> <dir>      every texture as .dds and .png
//   rk ytd build <dir> <out.ytd>   every .dds/.png in a folder into a dictionary
//   rk convert <in> <out>          a ytd/ydr/ydd/yft/ypt written for the selected edition (--enhanced / --legacy)
//
// The edition (game.h) is chosen before anything loads: --enhanced or --legacy anywhere on the command line (or
// RAGEKIT_EDITION=enhanced). Files are read in whichever edition they are; writers and the game index follow the choice.
#include <algorithm>
#include <array>
#include <map>
#include <cstdio>
#include <functional>
#include <cstring>
#include <filesystem>
#include <string>
#include "ragekit/ragekit.h"

namespace fs = std::filesystem;
using namespace rk;

static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string ext_of(const std::string& p) { return lower(fs::path(p).extension().string()); }

static void print_resource_header(const Bytes& file, const std::string& ext) {
    auto res = rsc::Resource::load(file);
    std::printf("RSC7 version %d (%s)\n", res.version, game_name(rsc::game_of(ext, res.version)));
    std::printf("  system   %9zu bytes  flags 0x%08X  pages %u  base %u\n", res.system.size(), res.system_flags.value,
                res.system_flags.page_count(), res.system_flags.base_size());
    std::printf("  graphics %9zu bytes  flags 0x%08X  pages %u  base %u\n", res.graphics.size(), res.graphics_flags.value,
                res.graphics_flags.page_count(), res.graphics_flags.base_size());
}

static void print_texture(const Texture& t, const char* indent) {
    std::printf("%s%-40s %5ux%-5u %-16s levels %2u  usage %2u flags 0x%06X  unk32 0x%02X  %zu bytes\n", indent, t.name.c_str(),
                t.width, t.height, texture_format_name(t.format), t.levels, (unsigned)t.usage(), t.usage_flags(), t.unk32, t.data.size());
}

static void print_drawable(const Drawable& d, bool verbose) {
    std::printf("Drawable '%s'\n", d.name.c_str());
    std::printf("  bbox (%.3f %.3f %.3f) .. (%.3f %.3f %.3f)  sphere (%.3f %.3f %.3f) r %.3f\n", d.bbox_min.x, d.bbox_min.y, d.bbox_min.z,
                d.bbox_max.x, d.bbox_max.y, d.bbox_max.z, d.bounding_center.x, d.bounding_center.y, d.bounding_center.z, d.bounding_radius);
    std::printf("  lod distances %.1f %.1f %.1f %.1f  masks/flags %08X %08X %08X %08X\n", d.lod_distance[0], d.lod_distance[1],
                d.lod_distance[2], d.lod_distance[3], d.render_mask_flags[0], d.render_mask_flags[1], d.render_mask_flags[2], d.render_mask_flags[3]);
    if (d.shader_group) {
        auto& sg = *d.shader_group;
        std::printf("  shader group: %zu shaders, %zu embedded textures\n", sg.shaders.size(), sg.textures ? sg.textures->textures.size() : 0);
        if (sg.textures) for (auto& t : sg.textures->textures) print_texture(t, "    ");
        for (size_t i = 0; i < sg.shaders.size(); i++) {
            auto& s = sg.shaders[i];
            std::printf("    [%zu] %s (%s) bucket %u, %zu params\n", i, hash_name(s.name_hash).c_str(), hash_name(s.sps_hash).c_str(),
                        s.render_bucket, s.params.size());
            if (verbose)
                for (auto& p : s.params) {
                    if (p.type == ParamType::Texture) std::printf("        %s = texture '%s' (slot %d)\n", hash_name(p.name_hash).c_str(), p.texture.c_str(), (int)p.slot);
                    else {
                        std::printf("        %s =", hash_name(p.name_hash).c_str());
                        for (auto& v : p.values) std::printf(" (%g %g %g %g)", v.x, v.y, v.z, v.w);
                        std::printf(" (slot %d)\n", (int)p.slot);
                    }
                }
        }
    }
    if (d.skeleton) {
        std::printf("  skeleton: %zu bones\n", d.skeleton->bones.size());
        if (verbose)
            for (auto& b : d.skeleton->bones)
                std::printf("    [%d] %-24s tag %5u parent %3d sibling %3d flags 0x%04X  t(%.3f %.3f %.3f)\n", b.index, b.name.c_str(), b.tag,
                            b.parent, b.next_sibling, b.flags, b.translation.x, b.translation.y, b.translation.z);
    }
    if (d.joints) std::printf("  joints: %zu rotation limits, %zu translation limits\n", d.joints->rotation_limits.size() / 0xC0, d.joints->translation_limits.size() / 0x40);
    static const char* lodnames[] = {"high", "med", "low", "vlow"};
    for (int l = 0; l < 4; l++) {
        if (d.lods[l].empty()) continue;
        std::printf("  lod %s: %zu models\n", lodnames[l], d.lods[l].size());
        for (auto& m : d.lods[l]) {
            std::printf("    model bone %u skin %d mask 0x%02X flags 0x%02X: %zu geometries\n", m.bone_index(), m.has_skin(), m.render_mask, m.flags, m.geometries.size());
            if (verbose)
                for (auto& g : m.geometries)
                    std::printf("      geom shader %u: %u verts (stride %u, flags 0x%04X, types %016llX), %zu indices, %zu bone ids\n", g.shader_index,
                                g.vertex_count, g.layout.stride(), g.layout.flags, (unsigned long long)g.layout.types, g.indices.size(), g.bone_ids.size());
        }
    }
    if (!d.lods_extra.empty()) std::printf("  extra model list: %zu models\n", d.lods_extra.size());
    if (!d.lights.empty()) std::printf("  lights: %zu\n", d.lights.size());
    if (d.bound) std::printf("  embedded bound\n");
}

static void print_bound(const Bound& b, int depth) {
    std::string ind((size_t)depth * 2, ' ');
    std::printf("%s%s mat %u room %u box (%.2f %.2f %.2f)..(%.2f %.2f %.2f) margin %.3f", ind.c_str(), bound_type_name(b.type), b.material_index,
                b.room_id(), b.box_min.x, b.box_min.y, b.box_min.z, b.box_max.x, b.box_max.y, b.box_max.z, b.margin);
    if (b.geometry) {
        auto& g = *b.geometry;
        size_t tris = 0;
        for (auto& p : g.polygons) if (p.kind == BoundPolygon::Triangle) tris++;
        std::printf("  %zu verts, %zu polys (%zu tris), %zu materials%s%s", g.vertices.size(), g.polygons.size(), tris, g.materials.size(),
                    g.bvh ? ", bvh" : "", g.has_octants ? ", octants" : "");
        if (g.bvh) std::printf(" (%u/%zu nodes, %zu trees)", g.bvh->node_count, g.bvh->nodes.size(), g.bvh->trees.size());
    }
    if (b.type == BoundType::Composite) std::printf("  %zu children%s%s", b.children.size(), b.composite_bvh ? ", bvh" : "", b.has_child_flags ? "" : ", no flags");
    std::printf("\n");
    for (auto& ch : b.children) {
        if (ch.bound) print_bound(*ch.bound, depth + 1);
        else std::printf("%s  (null child)\n", ind.c_str());
    }
}

static void print_fragment(const Fragment& f, bool verbose) {
    std::printf("Fragment '%s'  sphere (%.3f %.3f %.3f) r %.3f  gravity %.2f buoyancy %.2f  %zu glass windows, %zu cloths, %zu lights\n", f.name.c_str(),
                f.sphere_center.x, f.sphere_center.y, f.sphere_center.z, f.sphere_radius, f.gravity_factor, f.buoyancy_factor, f.glass_windows.size(),
                f.cloths.size(), f.lights.size());
    if (f.drawable) { std::printf("Main "); print_drawable(*f.drawable, verbose); if (f.drawable->bound) print_bound(*f.drawable->bound, 1); }
    if (f.drawable_cloth) { std::printf("Cloth "); print_drawable(*f.drawable_cloth, verbose); }
    for (size_t i = 0; i < f.drawable_array.size(); i++) { std::printf("Array[%zu] '%s' ", i, f.drawable_array_names[i].c_str()); print_drawable(f.drawable_array[i], verbose); }
    std::printf("bone transforms: %zu\n", f.bone_transforms.size());
    for (int l = 0; l < 3; l++) {
        if (!f.physics[l]) continue;
        auto& lod = *f.physics[l];
        std::printf("Physics LOD%d: %zu groups, %zu children, %zu transforms, archetype '%s'%s\n", l + 1, lod.groups.size(), lod.children.size(), lod.transforms.size(),
                    lod.archetype ? lod.archetype->name.c_str() : "", lod.archetype_damaged ? " (+damaged)" : "");
        for (size_t i = 0; i < lod.groups.size(); i++) {
            auto& g = lod.groups[i];
            std::printf("  group[%zu] %-24s parent %3u children %u@%u glass %u/%u mass %.2f\n", i, g.name().c_str(), g.parent_index(), g.child_count(), g.child_index(),
                        g.glass_window_index(), g.glass_flags(), g.mass());
        }
        for (size_t i = 0; i < lod.children.size(); i++) {
            auto& ch = lod.children[i];
            std::printf("  child[%zu] group %u bone %u mass %.2f/%.2f%s%s\n", i, ch.group_index, ch.bone_tag, ch.pristine_mass, ch.damaged_mass,
                        ch.drawable ? " drawable" : "", ch.drawable_damaged ? " +damaged" : "");
            if (verbose && ch.drawable) print_drawable(*ch.drawable, false);
        }
        if (lod.bound) print_bound(*lod.bound, 1);
    }
}

static void print_meta(const Meta& m, bool verbose) {
    std::printf("Meta '%s': %zu structure infos, %zu enum infos, %zu blocks, root block %d\n", m.name.c_str(), m.structs.size(),
                m.enums.size(), m.blocks.size(), m.root_block);
    for (auto& s : m.structs) {
        std::printf("  struct %-10s size %4d key %-10u unk8 %u, %zu entries\n", hash_name(s.name).c_str(), s.size, s.key, s.unknown8, s.entries.size());
        if (verbose)
            for (auto& e : s.entries)
                std::printf("      +%-4d %-10s type 0x%02X unk9 %u ref %d key %s\n", e.offset, hash_name(e.name).c_str(), e.type, e.unknown9, e.ref_index, hash_name(e.ref_key).c_str());
    }
    for (auto& e : m.enums) std::printf("  enum %s, %zu values\n", hash_name(e.name).c_str(), e.entries.size());
    for (size_t i = 0; i < m.blocks.size(); i++)
        std::printf("  block %zu: type %s, %zu bytes\n", i + 1, hash_name(m.blocks[i].type).c_str(), m.blocks[i].data.size());
}

static int cmd_gltf(const std::vector<std::string>& args) {
    // rk gltf <in> <out.glb> [texture.ytd ...] [--all-lods]
    std::string in = args[0], out = args[1];
    GltfOptions opt;
    std::vector<TextureDictionary> txds;
    for (size_t i = 2; i < args.size(); i++) {
        if (args[i] == "--all-lods") opt.all_lods_as_nodes = true;
        else if (args[i] == "--no-textures") opt.textures = false;
        else txds.push_back(read_ytd(rsc::read_file(args[i])));
    }
    opt.find_texture = [&](std::string_view name) -> const Texture* {
        for (auto& t : txds) if (auto* tx = t.find(name)) return tx;
        return nullptr;
    };
    Bytes file = rsc::read_file(in);
    std::string ext = ext_of(in);
    Bytes glb;
    if (ext == ".ydr") glb = write_glb(read_ydr(file), opt);
    else if (ext == ".yft") glb = write_glb(read_yft(file), opt);
    else if (ext == ".ydd") {
        auto dd = read_ydd(file);
        std::vector<GltfInstance> inst;
        for (auto& d : dd.drawables) inst.push_back({&d, Mat4::identity(), d.name});
        glb = write_glb(inst, opt);
    } else { std::printf("gltf: unsupported input %s\n", ext.c_str()); return 1; }
    rsc::write_file(out, glb);
    std::printf("wrote %s (%zu bytes)\n", out.c_str(), glb.size());
    return 0;
}

static int cmd_psodump(const std::string& path, size_t max_items, bool flat) {
    Bytes file = rsc::read_file(path);
    if (!is_pso(file)) { std::printf("%s: not a PSO file\n", path.c_str()); return 1; }
    Pso p = read_pso(file);
    std::fputs(pso_dump(p, max_items, flat).c_str(), stdout);
    return 0;
}

// rk ymf <in.ymf> [out.ymf]: lists a streaming manifest, optionally rewrites it with write_ymf
static int cmd_ymf(const std::vector<std::string>& args) {
    Bytes file = rsc::read_file(args[0]);
    Manifest m = read_ymf(file);
    std::printf("map data groups %zu, hd txd bindings %zu, imap dependencies %zu\n", m.map_data_groups.size(), m.hd_txd_bindings.size(), m.imap_dependencies.size());
    for (auto& d : m.imap_dependencies_2) { std::printf("imapDependencies_2 %s flags %u:", pso_name(d.name).c_str(), d.flags); for (auto h : d.deps) std::printf(" %s", pso_name(h).c_str()); std::printf("\n"); }
    for (auto& d : m.ityp_dependencies_2) { std::printf("itypDependencies_2 %s flags %u:", pso_name(d.name).c_str(), d.flags); for (auto h : d.deps) std::printf(" %s", pso_name(h).c_str()); std::printf("\n"); }
    for (auto& i : m.interiors) { std::printf("interior %s:", pso_name(i.name).c_str()); for (auto h : i.bounds) std::printf(" %s", pso_name(h).c_str()); std::printf("\n"); }
    if (args.size() > 1) {
        Bytes w = write_ymf(m);
        rsc::write_file(args[1], w);
        std::printf("wrote %s (%zu bytes, %s the original)\n", args[1].c_str(), w.size(), w == file ? "identical to" : "different from");
    }
    return 0;
}

// rk ymf-make <out.ymf> <interior> <ytyp-name> <ybn-name> [<ytyp-dependency>...]: the manifest an interior resource needs
static int cmd_ymf_make(const std::vector<std::string>& args) {
    Manifest m;
    std::vector<std::string> deps(args.begin() + 4, args.end());
    m.add_ityp_dependency(args[2], deps);
    m.add_interior(args[1], {args[3]});
    Bytes w = write_ymf(m);
    rsc::write_file(args[0], w);
    std::printf("wrote %s (%zu bytes)\n", args[0].c_str(), w.size());
    return 0;
}

// rk fxc <file.fxc>: the effect's constant buffers, variables (type, register, defaults, annotations) and techniques
static int cmd_fxc(const std::string& path) {
    Bytes file = rsc::read_file(path);
    FxcFile f = read_fxc(file);
    std::printf("%s: vertex type %u\n", path.c_str(), f.vertex_type);
    for (auto& [n, v] : f.preset_params) std::printf("  preset %s = %u\n", n.c_str(), v);
    for (auto& c : f.cbuffers) std::printf("  cbuffer %-24s %4u bytes  vs %u ps %u\n", c.name.c_str(), c.size, c.slot[0], c.slot[1]);
    for (auto& v : f.variables) {
        const FxcCBuffer* cb = f.cbuffer(v.cbuffer);
        std::printf("  var %-28s type %2u count %u slot %3u group %u off %3u variant %u name2 %-16s cb %s", v.name.c_str(), (unsigned)v.type, v.count, v.slot, v.group,
                    v.offset, v.variant, v.name2.c_str(), cb ? cb->name.c_str() : (v.cbuffer ? "?" : "-"));
        if (!v.values_f.empty()) { std::printf(" ="); for (float x : v.values_f) std::printf(" %g", x); }
        if (!v.values_u.empty()) { std::printf(" ="); for (uint32_t x : v.values_u) std::printf(" %u", x); }
        for (auto& a : v.annotations) {
            if (a.type == 2) std::printf(" [%s=%s]", a.name.c_str(), a.s.c_str());
            else if (a.type == 1) std::printf(" [%s=%g]", a.name.c_str(), a.f);
            else std::printf(" [%s=%d]", a.name.c_str(), a.i);
        }
        std::printf("\n");
    }
    for (auto& t : f.techniques) std::printf("  technique %s (%zu passes)\n", t.name.c_str(), t.passes.size());
    return 0;
}

static int cmd_info(const std::string& path, bool verbose) {
    Bytes file = rsc::read_file(path);
    if (is_rbf(file)) {   // creature metadata .ymt, cutscene / Bink metadata ...
        std::printf("RBF: %zu bytes\n%s", file.size(), rbf_dump(read_rbf(file)).c_str());
        return 0;
    }
    if (is_pso(file)) { // binary .ymt / .ypdb ...
        Pso p = read_pso(file);
        std::printf("PSO: %zu bytes\n%s", file.size(), pso_describe(p, verbose).c_str());
        if (p.has_map && p.has_schema) {
            PsoView v(p);
            PsoObject r = v.root();
            std::printf("  root: %s", r ? pso_name(r.type).c_str() : "(none)");
            if (r) std::printf(", %zu fields", v.fields(r).size());
            std::printf("\n");
        }
        return 0;
    }
    if (!rsc::Resource::is_resource(file)) { std::printf("%s: not an RSC7 resource\n", path.c_str()); return 1; }
    print_resource_header(file, ext_of(path));
    std::string ext = ext_of(path);
    if (ext == ".ytd") {
        auto txd = read_ytd(file);
        std::printf("TextureDictionary: %zu textures\n", txd.textures.size());
        for (auto& t : txd.textures) print_texture(t, "  ");
    } else if (ext == ".ydr") {
        print_drawable(read_ydr(file), verbose);
    } else if (ext == ".ydd") {
        auto dd = read_ydd(file);
        std::printf("DrawableDictionary: %zu drawables\n", dd.drawables.size());
        for (size_t i = 0; i < dd.drawables.size(); i++) {
            std::printf("[%s] ", hash_name(dd.hashes[i]).c_str());
            print_drawable(dd.drawables[i], verbose);
        }
    } else if (ext == ".ybn") {
        auto b = read_ybn(file);
        if (b) print_bound(*b, 0);
    } else if (ext == ".yft") {
        print_fragment(read_yft(file), verbose);
    } else if (ext == ".ypt") {
        const ParticleDictionary p = read_ypt(file);
        std::printf("Particle dictionary '%s': %zu textures, %zu drawables, %zu particle rules, %zu effect rules, %zu emitter rules, %zu blocks\n",
                    p.name.c_str(), p.textures ? p.textures->textures.size() : 0, p.drawables.size(), p.particle_rules.rules.size(),
                    p.effect_rules.rules.size(), p.emitter_rules.rules.size(), p.blocks().size());
        if (verbose) {
            if (p.textures) for (auto& t : p.textures->textures) print_texture(t, "  ");
            const std::pair<const char*, const PtxRuleList*> lists[] = {{"particle", &p.particle_rules}, {"effect", &p.effect_rules}, {"emitter", &p.emitter_rules}};
            for (auto& [kind, l] : lists)
                for (size_t i = 0; i < l->rules.size(); i++) std::printf("  %s rule %s\n", kind, l->name(i).c_str());
        }
    } else if (ext == ".ytyp" || ext == ".ymap") {
        print_meta(read_meta(file), verbose);
    } else {
        std::printf("(no reader for %s yet)\n", ext.c_str());
    }
    return 0;
}

static bool g_edition_chosen = false;   // --enhanced / --legacy given: write in it; else keep each file's own

static int cmd_roundtrip(const std::string& in, const std::string& out) {
    Bytes file = rsc::read_file(in);
    std::string ext = ext_of(in);
    Bytes written;
    if (!g_edition_chosen && rsc::Resource::is_resource(file)) set_default_game(rsc::game_of(ext, rsc::Resource::load(file).version));
    if (is_pso(file)) { // binary .ymt / .ypdb
        written = write_pso(read_pso(file));
        rsc::write_file(out, written);
        std::printf("%s: PSO in %zu bytes -> out %zu bytes (%s)\n", fs::path(in).filename().string().c_str(), file.size(), written.size(), written == file ? "identical" : "DIFFERENT");
        return written == file ? 0 : 1;
    }
    if (ext == ".ytd") written = write_ytd(read_ytd(file));
    else if (ext == ".ydr") written = write_ydr(read_ydr(file));
    else if (ext == ".ydd") written = write_ydd(read_ydd(file));
    else if (ext == ".ybn") written = write_ybn(*read_ybn(file));
    else if (ext == ".ytyp") written = write_ytyp(read_ytyp(file));
    else if (ext == ".ymap") written = write_ymap(read_ymap(file));
    else if (ext == ".yft") written = write_yft(read_yft(file));
    else if (ext == ".yld") written = write_yld(read_yld(file));
    else if (ext == ".ypt") written = write_ypt(read_ypt(file));
    else { std::printf("no writer for %s\n", ext.c_str()); return 1; }
    rsc::write_file(out, written);
    auto a = rsc::Resource::load(file), b = rsc::Resource::load(written);
    std::printf("%s: in %zu bytes (sys %zu gfx %zu) -> out %zu bytes (sys %zu gfx %zu)\n", fs::path(in).filename().string().c_str(), file.size(),
                a.system.size(), a.graphics.size(), written.size(), b.system.size(), b.graphics.size());
    // read the output back so a broken write fails loudly
    if (ext == ".ytd") read_ytd(written);
    else if (ext == ".ydr") read_ydr(written);
    else if (ext == ".ybn") read_ybn(written);
    else if (ext == ".ytyp") read_ytyp(written);
    else if (ext == ".ymap") read_ymap(written);
    else if (ext == ".yft") read_yft(written);
    else if (ext == ".yld") read_yld(written);
    else if (ext == ".ypt") read_ypt(written);
    else read_ydd(written);
    return 0;
}

// rk metaout <in-dir> <out-dir> [--generic]: writes every .ytyp/.ymap under in-dir (typed, or the generic
// container with --generic) into out-dir with the same relative paths, plus out-dir/pairs.txt ("orig|written"
// per line) for the CodeWalker oracle.
static int cmd_metaout(const std::string& in, const std::string& out, bool generic) {
    std::string pairs;
    int n = 0, failed = 0;
    for (auto& e : fs::recursive_directory_iterator(in)) {
        if (!e.is_regular_file()) continue;
        std::string ext = ext_of(e.path().string());
        if (ext != ".ytyp" && ext != ".ymap") continue;
        fs::path rel = fs::relative(e.path(), in);
        fs::path dst = fs::path(out) / rel;
        fs::create_directories(dst.parent_path());
        try {
            Bytes file = rsc::read_file(e.path().string());
            Bytes w = generic ? write_meta(read_meta(file)) : ext == ".ytyp" ? write_ytyp(read_ytyp(file)) : write_ymap(read_ymap(file));
            rsc::write_file(dst.string(), w);
            pairs += e.path().generic_string() + "|" + dst.generic_string() + "\n";
            n++;
        } catch (const std::exception& ex) {
            std::printf("FAIL %s: %s\n", e.path().string().c_str(), ex.what());
            failed++;
        }
    }
    FILE* f = std::fopen((fs::path(out) / "pairs.txt").string().c_str(), "wb");
    if (f) { std::fwrite(pairs.data(), 1, pairs.size(), f); std::fclose(f); }
    std::printf("wrote %d files (%d failed) to %s\n", n, failed, out.c_str());
    return failed ? 1 : 0;
}

static int cmd_ytd_export(const std::string& in, const std::string& dir) {
    auto txd = read_ytd(rsc::read_file(in));
    fs::create_directories(dir);
    for (auto& t : txd.textures) {
        std::string base = (fs::path(dir) / t.name).string();
        rsc::write_file(base + ".dds", write_dds(t));
        try {
            rsc::write_file(base + ".png", write_png(decode_texture(t)));
        } catch (const std::exception& e) {
            std::printf("  %s: png skipped (%s)\n", t.name.c_str(), e.what());
        }
        print_texture(t, "  ");
    }
    return 0;
}

// xml: rk xml <file.ydr|ydd|ybn|ytd> <out folder>: CodeWalker's "Export XML" (<name>.<ext>.xml + a <name> folder of DDS files)
static int cmd_xml(const std::string& in, const std::string& out_dir) {
    Bytes file = rsc::read_file(in);
    std::string ext = ext_of(in);
    std::string fname = fs::path(in).filename().string(), stem = fs::path(in).stem().string();
    fs::create_directories(out_dir);
    std::string xml;
    std::vector<std::pair<std::string, Bytes>> dds;
    if (ext == ".ydr") { auto d = read_ydr(file); xml = write_drawable_xml(d); dds = dds_files(d); }
    else if (ext == ".ydd") { auto d = read_ydd(file); xml = write_drawable_dictionary_xml(d); dds = dds_files(d); }
    else if (ext == ".ytd") { auto t = read_ytd(file); xml = write_texture_dictionary_xml(t); dds = dds_files(t); }
    else if (ext == ".ybn") { auto b = read_ybn(file); if (!b) throw Error("empty ybn"); xml = write_bounds_xml(*b); }
    else { std::fprintf(stderr, "rk xml: %s is not a .ydr/.ydd/.ybn/.ytd\n", in.c_str()); return 1; }
    rsc::write_file((fs::path(out_dir) / (fname + ".xml")).string(), Bytes(xml.begin(), xml.end()));
    if (!dds.empty()) fs::create_directories(fs::path(out_dir) / stem);
    for (auto& [name, bytes] : dds) rsc::write_file((fs::path(out_dir) / stem / name).string(), bytes);
    std::printf("%s.xml (%zu bytes), %zu dds\n", fname.c_str(), xml.size(), dds.size());
    return 0;
}

static int cmd_ytd_build(const std::string& dir, const std::string& out) {
    TextureDictionary txd;
    for (auto& e : fs::directory_iterator(dir)) {
        std::string ext = lower(e.path().extension().string());
        std::string name = e.path().stem().string();
        if (ext == ".dds") txd.add(read_dds(rsc::read_file(e.path().string()), name));
        else if (ext == ".png") {
            Image img = read_png(rsc::read_file(e.path().string()));
            bool alpha = false;
            for (size_t i = 3; i < img.rgba.size(); i += 4) if (img.rgba[i] != 255) { alpha = true; break; }
            txd.add(encode_texture(img, alpha ? TextureFormat::DXT5 : TextureFormat::DXT1, name));
        }
    }
    rsc::write_file(out, write_ytd(txd));
    std::printf("wrote %s with %zu textures\n", out.c_str(), txd.textures.size());
    return 0;
}

// ---- rk rpf -----------------------------------------------------------------------------

static std::string sha256_hex(const uint8_t* data, size_t len) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto ror = [](uint32_t v, int n) { return (v >> n) | (v << (32 - n)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };
    size_t full = len / 64;
    for (size_t i = 0; i < full; i++) block(data + 64 * i);
    uint8_t tail[128] = {};
    size_t rem = len - full * 64;
    if (rem) std::memcpy(tail, data + full * 64, rem);
    tail[rem] = 0x80;
    size_t tl = rem + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < tl; i += 64) block(tail + i);
    char out[65];
    for (int i = 0; i < 8; i++) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return out;
}

/// SHA256 of what CodeWalker's RpfFile.ExtractFile returns: binary files as extracted, resources
/// inflated (without the 16 byte RSC7 header). Also counts on-disk RSC7 headers that differ from ours (retail archives keep scrambled bytes there, only FiveM/OpenIV archives hold real ones).
static std::string rpf_hash(const Rpf& rpf, int idx, int* header_mismatch) {
    const RpfEntry& e = rpf.entries()[(size_t)idx];
    try {
        Bytes data = rpf.extract(idx);
        if (e.is_resource()) {
            if (data.size() < 16) return data.empty() ? "empty" : "short";
            Bytes raw = rpf.extract_raw(idx);
            if (header_mismatch && raw.size() >= 16 && std::memcmp(raw.data(), data.data(), 4) == 0 && std::memcmp(raw.data(), data.data(), 16) != 0) (*header_mismatch)++;
            Bytes body;
            // like CodeWalker: a stream that does not inflate is returned as stored
            try { body = rsc::inflate(std::span<const uint8_t>(data).subspan(16), e.size() + 16); } catch (...) { body.assign(data.begin() + 16, data.end()); }
            return sha256_hex(body.data(), body.size());
        }
        return sha256_hex(data.data(), data.size());
    } catch (const std::exception& ex) {
        return std::string("ERR:") + ex.what();
    }
}

static const char* rpf_kind(const RpfEntry& e) { return e.is_directory() ? "D" : e.is_resource() ? "R" : "B"; }

static bool g_rpf_hash = false;
static int g_rpf_header_mismatch = 0;

static void rpf_print(const Rpf& rpf, int idx, bool tsv, const std::string& prefix) {
    const RpfEntry& e = rpf.entries()[(size_t)idx];
    std::string path = prefix + e.path;
    if (tsv) {
        std::printf("%s\t%s\t%llu\t%u\t%u\t%u\t%u\t%d", rpf_kind(e), path.c_str(), (unsigned long long)e.size(), e.stored_size,
                    e.uncompressed_size, e.system_flags, e.graphics_flags, e.encrypted ? 1 : 0);
        if (g_rpf_hash) std::printf("\t%s", e.is_directory() ? "-" : rpf_hash(rpf, idx, &g_rpf_header_mismatch).c_str());
        std::printf("\n");
        return;
    }
    if (e.is_directory()) std::printf("D %12s %-12s  %s/\n", "", "", path.c_str());
    else if (e.is_resource())
        std::printf("R %12llu %-12u  %s  (v%u sys 0x%08X gfx 0x%08X%s)\n", (unsigned long long)e.size(), e.stored_size, path.c_str(),
                    e.resource_version(), e.system_flags, e.graphics_flags, e.encrypted ? " encrypted" : "");
    else
        std::printf("B %12llu %-12u  %s%s%s\n", (unsigned long long)e.size(), e.stored_size, path.c_str(), e.compressed() ? "  (deflate)" : "",
                    e.encrypted ? " encrypted" : "");
}

static void rpf_walk(const Rpf& rpf, int dir, bool recursive, bool nested, bool tsv, const std::string& prefix) {
    for (int c : rpf.children(dir)) {
        const RpfEntry& e = rpf.entries()[(size_t)c];
        rpf_print(rpf, c, tsv, prefix);
        if (e.is_directory() && recursive) rpf_walk(rpf, c, recursive, nested, tsv, prefix);
        else if (e.is_archive() && nested) {
            try {
                Rpf child = rpf.open_child(c);
                rpf_walk(child, 0, true, true, tsv, prefix + e.path + "/");
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "%s%s: %s\n", prefix.c_str(), e.path.c_str(), ex.what());
            }
        }
    }
}

static const char* rpf_enc_name(RpfEncryption e) {
    switch (e) {
        case RpfEncryption::None: return "none";
        case RpfEncryption::Open: return "open (unencrypted)";
        case RpfEncryption::Aes: return "AES";
        case RpfEncryption::Ng: return "NG";
    }
    return "unknown (treated as NG)";
}

static void rpf_extract_dir(const Rpf& rpf, int dir, const fs::path& out, bool nested, int& files, int& errors) {
    fs::create_directories(out);
    for (int c : rpf.children(dir)) {
        const RpfEntry& e = rpf.entries()[(size_t)c];
        fs::path target = out / e.name;
        try {
            if (e.is_directory()) rpf_extract_dir(rpf, c, target, nested, files, errors);
            else if (e.is_archive() && nested) {
                Rpf child = rpf.open_child(c);
                rpf_extract_dir(child, 0, target, nested, files, errors);
            } else {
                Bytes data = e.is_archive() ? rpf.extract_raw(c) : rpf.extract(c);
                rsc::write_file(target.string(), data);
                files++;
            }
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "error: %s: %s\n", e.path.c_str(), ex.what());
            errors++;
        }
    }
}

// ---- rk rpf pack / add / rm / rewrite ----

static void rpf_pack_into(RpfBuilder& b, const fs::path& dir, const std::string& prefix) {
    std::vector<fs::path> items;
    for (auto& e : fs::directory_iterator(dir)) items.push_back(e.path());
    std::sort(items.begin(), items.end());
    for (auto& p : items) {
        std::string name = p.filename().string();
        if (fs::is_directory(p)) {
            if (lower(p.extension().string()) == ".rpf") {
                RpfBuilder child;
                rpf_pack_into(child, p, "");
                b.add_archive(prefix + name, std::move(child));
            } else {
                b.add_directory(prefix + name);
                rpf_pack_into(b, p, prefix + name + "/");
            }
        } else b.add_file(prefix + name, rsc::read_file(p.string()));
    }
}

static int cmd_rpf_write(const std::string& sub, std::vector<std::string> args, bool aes, bool rewrite_nested) {
    RpfWriteOptions opt;
    if (aes) opt.encryption = RpfEncryption::Aes;
    if (sub == "pack" && args.size() == 2) {
        RpfBuilder b;
        rpf_pack_into(b, args[0], "");
        b.save(args[1], opt);
        std::printf("wrote %s (%zu entries)\n", args[1].c_str(), b.list().size());
        return 0;
    }
    if (sub == "rewrite" && args.size() == 2) { // read an archive and write it again (re-encrypting as OPEN/AES)
        {
            Rpf rpf = Rpf::open(args[0]);
            RpfBuilder b = RpfBuilder::from_archive(rpf, rewrite_nested ? RpfBuilder::Nested::Rewrite : RpfBuilder::Nested::Keep);
            b.save(args[1], opt);
        }
        std::printf("wrote %s\n", args[1].c_str());
        return 0;
    }
    if ((sub == "add" && args.size() == 3) || (sub == "rm" && args.size() == 2)) {
        std::string tmp = args[0] + ".tmp";
        {
            Rpf rpf = Rpf::open(args[0]);
            // a path through a nested archive ("x64/a.rpf/inner/file") edits that archive: load nested ones as builders
            bool through_nested = lower(args[1]).find(".rpf/") != std::string::npos;
            RpfBuilder b = RpfBuilder::from_archive(rpf, through_nested ? RpfBuilder::Nested::Rewrite : RpfBuilder::Nested::Keep);
            RpfBuilder* target = &b;
            std::string rest = args[1];
            for (;;) {
                size_t at = lower(rest).find(".rpf/");
                if (at == std::string::npos) break;
                RpfBuilder* n = target->nested(rest.substr(0, at + 4));
                if (!n) { std::fprintf(stderr, "error: no nested archive %s\n", rest.substr(0, at + 4).c_str()); return 1; }
                target = n;
                rest = rest.substr(at + 5);
            }
            if (sub == "add") target->add_file(rest, rsc::read_file(args[2]));
            else if (!target->remove(rest)) { std::fprintf(stderr, "error: no such entry: %s\n", args[1].c_str()); return 1; }
            if (rpf.encryption() == RpfEncryption::Aes) opt.encryption = RpfEncryption::Aes; // keep AES
            if (rpf.encryption() == RpfEncryption::Ng) { std::fprintf(stderr, "error: NG archives cannot be written; use `rk rpf rewrite` to make an OPEN copy\n"); return 1; }
            b.save(tmp, opt);
        }
        std::error_code ec;
        fs::remove(args[0], ec);
        fs::rename(tmp, args[0]);
        std::printf("%s: %s %s\n", args[0].c_str(), sub == "add" ? "added" : "removed", args[1].c_str());
        return 0;
    }
    return 2;
}

static int cmd_rpf(const std::vector<std::string>& args_in) {
    std::vector<std::string> args;
    bool recursive = false, nested = false, tsv = false, aes = false;
    std::string magic;
    for (size_t i = 0; i < args_in.size(); i++) {
        const std::string& a = args_in[i];
        if (a == "--magic" && i + 1 < args_in.size()) { magic = args_in[++i]; continue; }
        if (a == "-r") recursive = true;
        else if (a == "--nested") nested = true;
        else if (a == "--aes") aes = true;
        else if (a == "--tsv") tsv = true;
        else if (a == "--hash") g_rpf_hash = true;
        else args.push_back(a);
    }
    if (args.empty()) return 2;
    const std::string& sub = args[0];
    if (sub == "keys" && (args.size() == 2 || args.size() == 3)) {
        std::string folder = args.size() == 3 ? args[2] : RpfKeys::default_folder();
        RpfKeys k = RpfKeys::generate(args[1], [](const std::string& s) { std::printf("%s\n", s.c_str()); }, magic);
        fs::create_directories(folder);
        k.save(folder);
        std::printf("keys saved to %s (never commit them)\n", folder.c_str());
        return 0;
    }
    if (sub == "pack" || sub == "add" || sub == "rm" || sub == "rewrite")
        return cmd_rpf_write(sub, std::vector<std::string>(args.begin() + 1, args.end()), aes, nested);
    if (args.size() < 2) return 2;
    Rpf rpf = Rpf::open(args[1]);
    if (sub == "ls" && args.size() <= 3) {
        Rpf::Location loc;
        loc.archive = std::make_shared<Rpf>(rpf);
        loc.entry = 0;
        if (args.size() == 3 && !args[2].empty() && args[2] != "/") loc = rpf.resolve(args[2]);
        if (!loc.archive) { std::fprintf(stderr, "error: no such entry: %s\n", args[2].c_str()); return 1; }
        if (!tsv)
            std::printf("%s: %llu bytes, %zu entries, encryption %s\n", rpf.name().c_str(), (unsigned long long)rpf.byte_size(),
                        rpf.entries().size(), rpf_enc_name(rpf.encryption()));
        const RpfEntry& e = loc.archive->entries()[(size_t)loc.entry];
        std::string prefix;
        if (e.is_directory()) {
            rpf_walk(*loc.archive, loc.entry, recursive, nested, tsv, prefix);
        } else if (e.is_archive()) {
            Rpf child = loc.archive->open_child(loc.entry);
            rpf_walk(child, 0, recursive, nested, tsv, "");
        } else rpf_print(*loc.archive, loc.entry, tsv, "");
        if (g_rpf_hash) std::fprintf(stderr, "readable on-disk RSC7 headers differing from the entry flags/version: %d\n", g_rpf_header_mismatch);
        return 0;
    }
    if (sub == "cat" && args.size() == 4) {
        auto loc = rpf.resolve(args[2]);
        if (!loc.archive) { std::fprintf(stderr, "error: no such entry: %s\n", args[2].c_str()); return 1; }
        const RpfEntry& e = loc.archive->entries()[(size_t)loc.entry];
        Bytes data = e.is_archive() ? loc.archive->extract_raw(loc.entry) : loc.archive->extract(loc.entry);
        rsc::write_file(args[3], data);
        std::printf("wrote %s (%zu bytes)\n", args[3].c_str(), data.size());
        return 0;
    }
    if (sub == "extract" && (args.size() == 3 || args.size() == 4)) {
        int files = 0, errors = 0;
        Rpf::Location loc;
        loc.archive = std::make_shared<Rpf>(rpf);
        loc.entry = 0;
        if (args.size() == 4) loc = rpf.resolve(args[3]);
        if (!loc.archive) { std::fprintf(stderr, "error: no such entry: %s\n", args[3].c_str()); return 1; }
        const RpfEntry& e = loc.archive->entries()[(size_t)loc.entry];
        if (e.is_directory()) rpf_extract_dir(*loc.archive, loc.entry, args[2], nested, files, errors);
        else if (e.is_archive()) rpf_extract_dir(loc.archive->open_child(loc.entry), 0, args[2], nested, files, errors);
        else { fs::create_directories(args[2]); rsc::write_file((fs::path(args[2]) / e.name).string(), loc.archive->extract(loc.entry)); files++; }
        std::printf("extracted %d files, %d errors\n", files, errors);
        return errors ? 1 : 0;
    }
    return 2;
}

// ---- rk rel ------------------------------------------------------------------------------------
// Walks an rpf (nested archives included) or a folder, calling fn(path, bytes) for every *.rel.
template <class F> static void rel_walk_rpf(const Rpf& rpf, const std::string& prefix, F& fn) {
    for (size_t i = 1; i < rpf.entries().size(); i++) {
        const RpfEntry& e = rpf.entries()[i];
        if (e.is_directory()) continue;
        if (e.is_archive()) {
            try { Rpf child = rpf.open_child((int)i); rel_walk_rpf(child, prefix + e.path + "/", fn); }
            catch (const std::exception& ex) { std::fprintf(stderr, "%s%s: %s\n", prefix.c_str(), e.path.c_str(), ex.what()); }
        } else if (ext_of(e.name) == ".rel") {
            try { fn(prefix + e.path, rpf.extract((int)i)); }
            catch (const std::exception& ex) { std::fprintf(stderr, "%s%s: %s\n", prefix.c_str(), e.path.c_str(), ex.what()); }
        }
    }
}

static int cmd_rel_scan(const std::vector<std::string>& args) {
    // rk rel scan <out dir | -> <rpf|folder>...   round-trips every .rel, copies them to out dir (flattened names)
    std::string out = args[0];
    if (out != "-") fs::create_directories(out);
    std::map<uint32_t, std::array<int, 3>> counts; // type -> {files, identical, failed}
    int unreadable = 0;
    auto test = [&](const std::string& path, const Bytes& bytes) {
        static int seq = 0;
        std::string flat = path.find(".rpf/") == std::string::npos ? std::to_string(seq++) + "_" + fs::path(path).filename().string() : path;
        for (auto& c : flat) if (c == '/' || c == '\\' || c == ':') c = '~';
        if (out != "-") rsc::write_file((fs::path(out) / flat).string(), bytes);
        auto pair_line = [&](const Bytes& w) {
            if (out == "-") return;
            fs::create_directories(fs::path(out) / "w");
            rsc::write_file((fs::path(out) / "w" / flat).string(), w);
            std::FILE* f = std::fopen((fs::path(out) / "pairs.txt").string().c_str(), "ab");
            if (f) { std::fprintf(f, "%s|%s\n", (fs::path(out) / flat).generic_string().c_str(), (fs::path(out) / "w" / flat).generic_string().c_str()); std::fclose(f); }
        };
        uint32_t type = bytes.size() >= 4 ? uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24 : 0;
        auto& c = counts[type];
        c[0]++;
        try {
            Rel r = read_rel(bytes);
            Bytes w = write_rel(r);
            pair_line(w);
            std::string v = r.validate();
            if (w == bytes && v.empty()) { c[1]++; return; }
            c[2]++;
            std::printf("DIFF %s: %s%s\n", path.c_str(), w == bytes ? "" : "bytes differ ", v.c_str());
        } catch (const std::exception& ex) {
            c[2]++; unreadable++;
            std::printf("FAIL %s: %s\n", path.c_str(), ex.what());
        }
    };
    for (size_t i = 1; i < args.size(); i++) {
        fs::path p = args[i];
        if (ext_of(args[i]) == ".rpf") {
            Rpf rpf = Rpf::open(args[i]);
            rel_walk_rpf(rpf, p.filename().string() + "/", test);
        } else if (fs::is_directory(p)) {
            for (auto& d : fs::recursive_directory_iterator(p))
                if (d.is_regular_file() && ext_of(d.path().string()) == ".rel") test(d.path().generic_string(), rsc::read_file(d.path().string()));
        } else if (ext_of(args[i]) == ".rel") test(args[i], rsc::read_file(args[i]));
    }
    int tf = 0, ti = 0, tx = 0;
    for (auto& [t, c] : counts) {
        std::printf("%-6s (type %3u): %5d files, %5d byte-identical, %d failed\n", rel_type_name(t), t, c[0], c[1], c[2]);
        tf += c[0]; ti += c[1]; tx += c[2];
    }
    std::printf("total: %d files, %d byte-identical, %d failed\n", tf, ti, tx);
    return tx ? 1 : 0;
}

static int cmd_rel(const std::vector<std::string>& args) {
    if (args.empty()) return 2;
    const std::string& sub = args[0];
    if (sub == "info" && args.size() >= 2) {
        size_t max = 0;
        if (args.size() >= 4 && args[2] == "--max") max = (size_t)std::atoi(args[3].c_str());
        Rel r = read_rel(rsc::read_file(args[1]));
        std::fputs(rel_describe(r, max).c_str(), stdout);
        std::string v = r.validate();
        if (!v.empty()) std::printf("INVALID: %s\n", v.c_str());
        return 0;
    }
    if (sub == "roundtrip" && args.size() == 3) {
        Bytes in = rsc::read_file(args[1]);
        Bytes out = write_rel(read_rel(in));
        rsc::write_file(args[2], out);
        std::printf("%s (%zu bytes)\n", out == in ? "byte-identical" : "DIFFERENT", out.size());
        return out == in ? 0 : 1;
    }
    if (sub == "scan" && args.size() >= 3) return cmd_rel_scan(std::vector<std::string>(args.begin() + 1, args.end()));
    return 2;
}

// ---- rk occlusion ------------------------------------------------------------------------------
static void print_occlusion(const OcclusionData& d, bool all) {
    std::printf("portal infos %zu, path nodes %zu\n", d.portals.size(), d.nodes.size());
    size_t n = 0;
    for (auto& p : d.portals) {
        if (!all && n++ >= 12) { std::printf("  ...\n"); break; }
        std::printf("  portal room %d idx %d -> room %d  proxy %08X", p.room_idx, p.portal_idx, p.dest_room_idx, p.interior_proxy_hash);
        for (auto& e : p.entities) std::printf("  [entity %08X link %u occ %g%s%s]", e.model_hash, e.link_type, e.max_occlusion, e.is_door ? " door" : "", e.is_glass ? " glass" : "");
        std::printf("\n");
    }
    n = 0;
    for (auto& nd : d.nodes) {
        if (!all && n++ >= 12) { std::printf("  ...\n"); break; }
        std::printf("  node %08X:", nd.key);
        for (auto& c : nd.children) std::printf(" (%08X, portal %d)", c.path_node_key, c.portal_info_idx);
        std::printf("\n");
    }
}

static int cmd_occlusion(const std::vector<std::string>& args) {
    if (args.empty()) return 2;
    const std::string& sub = args[0];
    if (sub == "info" && args.size() >= 2) {
        print_occlusion(read_occlusion_ymt(rsc::read_file(args[1])), args.size() > 2 && args[2] == "--all");
        return 0;
    }
    if (sub == "roundtrip" && args.size() == 3) {
        OcclusionData d = read_occlusion_ymt(rsc::read_file(args[1]));
        Bytes out = write_occlusion_ymt(d);
        rsc::write_file(args[2], out);
        OcclusionData e = read_occlusion_ymt(out);
        bool same = e.portals.size() == d.portals.size() && e.nodes.size() == d.nodes.size();
        std::printf("%zu portal infos, %zu path nodes written (%zu bytes); read back %s\n", d.portals.size(), d.nodes.size(), out.size(), same ? "with the same counts" : "DIFFERENT");
        return same ? 0 : 1;
    }
    if (sub == "scan" && args.size() >= 3) {
        // rk occlusion scan <out dir> <occlusion.rpf | update.rpf ...>: every .ymt read and rewritten; pairs.txt lists original|written for `oracle pso`
        fs::path out = args[1];
        fs::create_directories(out / "w");
        int files = 0, failed = 0;
        std::FILE* pairs = std::fopen((out / "pairs.txt").string().c_str(), "wb");
        for (size_t i = 2; i < args.size(); i++) {
            Rpf rpf = Rpf::open(args[i]);
            std::function<void(const Rpf&, bool)> walk = [&](const Rpf& r, bool occ) {
                for (size_t k = 1; k < r.entries().size(); k++) {
                    const RpfEntry& e = r.entries()[k];
                    if (e.is_directory()) continue;
                    if (e.is_archive()) { try { walk(r.open_child((int)k), occ || lower(e.name).find("occlusion") != std::string::npos); } catch (...) {} continue; }
                    if (ext_of(e.name) != ".ymt" || !occ) continue;
                    files++;
                    try {
                        Bytes b = r.extract((int)k);
                        if (!is_pso(b)) continue;
                        OcclusionData d = read_occlusion_ymt(b);
                        fs::path a = out / e.name, w = out / "w" / e.name;
                        rsc::write_file(a.string(), b);
                        rsc::write_file(w.string(), write_occlusion_ymt(d));
                        std::fprintf(pairs, "%s|%s\n", a.generic_string().c_str(), w.generic_string().c_str());
                    } catch (const std::exception& ex) { failed++; std::printf("FAIL %s: %s\n", e.path.c_str(), ex.what()); }
                }
            };
            walk(rpf, lower(args[i]).find("occlusion") != std::string::npos);
        }
        std::fclose(pairs);
        std::printf("%d occlusion ymt files, %d failed\n", files, failed);
        return failed ? 1 : 0;
    }
    // rk occlusion <ytyp> <out dir> [--archetype <name>] [--name <name>] [--pos x,y,z | --ymap <file>] [--settings-from <dat151.rel>]
    if (args.size() >= 2) {
        std::string ytyp = args[0], outdir = args[1], arch, name, ymap, rel_name;
        Vec3 pos; bool have_pos = false, case_names = false;
        for (size_t i = 2; i < args.size(); i++) {
            if (args[i] == "--archetype" && i + 1 < args.size()) arch = args[++i];
            else if (args[i] == "--name" && i + 1 < args.size()) name = args[++i];
            else if (args[i] == "--ymap" && i + 1 < args.size()) ymap = args[++i];
            else if (args[i] == "--case-sensitive-names") case_names = true;
            else if (args[i] == "--rel-name" && i + 1 < args.size()) rel_name = args[++i];
            else if (args[i] == "--pos" && i + 1 < args.size()) {
                if (std::sscanf(args[++i].c_str(), "%f,%f,%f", &pos.x, &pos.y, &pos.z) != 3) { std::fprintf(stderr, "error: --pos wants x,y,z\n"); return 1; }
                have_pos = true;
            }
        }
        MapTypes t = read_ytyp(rsc::read_file(ytyp));
        const Archetype* mlo = nullptr;
        for (auto& a : t.archetypes) {
            if (!a.is_mlo()) continue;
            if (arch.empty() || a.name == joaat_lower(arch)) { mlo = &a; break; }
        }
        if (!mlo) { std::fprintf(stderr, "error: no MLO archetype%s in %s\n", arch.empty() ? "" : " of that name", ytyp.c_str()); return 1; }
        if (name.empty()) name = !arch.empty() ? arch : fs::path(ytyp).stem().string();
        if (joaat_lower(name) != mlo->name) { std::fprintf(stderr, "error: joaat(\"%s\") is not the MLO archetype hash %08X: pass --archetype or --name\n", name.c_str(), mlo->name); return 1; }
        if (!ymap.empty()) {
            MapData md = read_ymap(rsc::read_file(ymap));
            for (auto& e : md.entities) if (e.is_mlo_instance && e.archetype_name == mlo->name) { pos = e.position; have_pos = true; break; }
            if (!have_pos) { std::fprintf(stderr, "error: no instance of the interior in %s\n", ymap.c_str()); return 1; }
        }
        if (!have_pos) { std::fprintf(stderr, "error: the proxy hash needs the placement: --pos x,y,z or --ymap <file>\n"); return 1; }
        AudioOcclusion m = occlusion_from_mlo(*mlo, pos);
        m.name = name;
        fs::create_directories(fs::path(outdir) / "stream");
        fs::create_directories(fs::path(outdir) / "audio");
        uint32_t proxy = m.proxy_hash();
        OcclusionData d = build_occlusion(m);
        InteriorAudioSettings ias; ias.case_sensitive_names = case_names;
        fs::path ymt = fs::path(outdir) / "stream" / (std::to_string(proxy) + ".ymt");
        fs::path rel = fs::path(outdir) / "audio" / (rel_name.empty() ? interior_dat151_file_name(name) : rel_name);
        rsc::write_file(ymt.string(), write_occlusion_ymt(d));
        rsc::write_file(rel.string(), write_rel(build_interior_dat151(m, ias)));
        std::printf("%s: proxy %u (0x%08X), %zu rooms, %zu portals, %zu portal infos, %zu path nodes\n  %s\n  %s\n", name.c_str(), proxy, proxy,
                    m.rooms.size(), m.portals.size(), d.portals.size(), d.nodes.size(), ymt.string().c_str(), rel.string().c_str());
        std::printf("  rooms:");
        for (auto& r : m.rooms) std::printf(" %s", r.c_str());
        std::printf("\n");
        return 0;
    }
    return 2;
}
int cmd_gameindex(const std::string& cmd, const std::vector<std::string>& args); // tools/rk_gameindex.cpp

static int cmd_convert(const std::string& in, const std::string& out) {
    const Bytes file = rsc::read_file(in);
    const std::string ext = ext_of(in);
    const Game to = default_game();
    Bytes written;
    if (ext == ".ytd") written = write_ytd(read_ytd(file), to);
    else if (ext == ".ydr") written = write_ydr(read_ydr(file), to);
    else if (ext == ".ydd") written = write_ydd(read_ydd(file), to);
    else if (ext == ".yft") written = write_yft(read_yft(file), to);
    else if (ext == ".ypt") written = write_ypt(read_ypt(file), to);
    else { std::printf("%s: the editions share this file type; nothing to convert\n", in.c_str()); return 1; }
    rsc::write_file(out, written);
    const int from = rsc::Resource::load(file).version;
    std::printf("%s: %s (version %d) -> %s (version %d), %zu bytes\n", fs::path(in).filename().string().c_str(), game_name(rsc::game_of(ext, from)), from,
                game_name(to), rsc::Resource::load(written).version, written.size());
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (const char* e = std::getenv("RAGEKIT_EDITION")) if (auto g = game_from_name(e)) { set_default_game(*g); g_edition_chosen = true; }
        std::string cmd;
        bool verbose = false;
        std::vector<std::string> args;
        for (int i = 1; i < argc; i++) {
            const std::string a = argv[i];
            if (a == "--enhanced" || a == "--gen9") { set_default_game(Game::Enhanced); g_edition_chosen = true; }
            else if (a == "--legacy" || a == "--gen8") { set_default_game(Game::Legacy); g_edition_chosen = true; }
            else if (a.rfind("--edition=", 0) == 0) {
                auto g = game_from_name(a.substr(10));
                if (!g) throw Error("--edition takes legacy or enhanced");
                set_default_game(*g);
                g_edition_chosen = true;
            }
            else if (cmd.empty()) cmd = a;
            else if (a == "-v") verbose = true;
            else args.push_back(a);
        }
        if (cmd == "psodump" && !args.empty()) {
            size_t max_items = 0; bool flat = false;
            for (size_t i = 0; i + 1 < args.size(); i++) {
                if (args[i] == "--flat") flat = true;
                else if (args[i] == "--max" && i + 2 < args.size()) max_items = (size_t)std::atoi(args[++i].c_str());
            }
            return cmd_psodump(args.back(), max_items, flat);
        }
        if (cmd == "ymf" && (args.size() == 1 || args.size() == 2)) return cmd_ymf(args);
        if (cmd == "ymf-make" && args.size() >= 4) return cmd_ymf_make(args);
        if (cmd == "fxc" && args.size() == 1) return cmd_fxc(args[0]);
        if (cmd == "info" && args.size() == 1) return cmd_info(args[0], verbose);
        if (cmd == "dump" && args.size() == 1) return cmd_info(args[0], true);
        if (cmd == "roundtrip" && args.size() == 2) return cmd_roundtrip(args[0], args[1]);
        if (cmd == "convert" && args.size() == 2) {
            if (!g_edition_chosen) throw Error("rk convert: choose the edition to write with --enhanced or --legacy");
            return cmd_convert(args[0], args[1]);
        }
        if (cmd == "metaout" && (args.size() == 2 || (args.size() == 3 && args[2] == "--generic"))) return cmd_metaout(args[0], args[1], args.size() == 3);
        if (cmd == "ytd" && args.size() == 3 && args[0] == "export") return cmd_ytd_export(args[1], args[2]);
        if (cmd == "ytd" && args.size() == 3 && args[0] == "build") return cmd_ytd_build(args[1], args[2]);
        if (cmd == "xml" && args.size() == 2) return cmd_xml(args[0], args[1]);
        if (cmd == "gltf" && args.size() >= 2) return cmd_gltf(args);
        if (cmd == "occlusion") { int rc = cmd_occlusion(args); if (rc != 2) return rc; }
        if (cmd == "rel") { int rc = cmd_rel(args); if (rc != 2) return rc; }
        if (cmd == "rpf") { int rc = cmd_rpf(args); if (rc != 2) return rc; }
        if (cmd == "index" || cmd == "find" || cmd == "pull" || cmd == "shaders" || cmd == "world") {
            std::vector<std::string> a = args;
            if (verbose) a.push_back("-v");
            int rc = cmd_gameindex(cmd, a);
            if (rc != 2) return rc;
        }
        std::printf("usage (any command takes --enhanced or --legacy: the edition written and the install the index opens; default legacy):\n"
                    "  rk convert <in.ytd|ydr|ydd|yft|ypt> <out> --enhanced|--legacy\n  rk xml <file.ydr|ydd|ybn|ytd> <out folder>\n  rk info <file> [-v]\n  rk dump <file>\n  rk roundtrip <in> <out>\n  rk ytd export <ytd> <dir>\n  rk ytd build <dir> <out.ytd>\n"
                    "  rk gltf <ydr|ydd|yft> <out.glb> [textures.ytd ...] [--all-lods] [--no-textures]\n"
                    "  rk rpf ls <file.rpf> [path] [-r] [--nested] [--tsv [--hash]]\n  rk rpf cat <file.rpf> <entry path> <out>\n  rk rpf extract <file.rpf> <out dir> [path] [--nested]\n"
                    "  rk rpf keys <GTA5.exe> [key folder] [--magic <magic.dat>]\n  rk rpf pack <dir> <out.rpf> [--aes]\n  rk rpf add <rpf> <entry path> <file>\n"
                    "  rk rel info <file.rel> [--max N]\n  rk rel roundtrip <in.rel> <out.rel>\n  rk rel scan <out dir|-> <rpf|folder|file.rel>...\n"
                    "  rk occlusion <ytyp> <out dir> [--archetype <name>] [--name <name>] [--pos x,y,z | --ymap <file>]\n"
                    "  rk occlusion info <ymt> [--all]\n  rk occlusion roundtrip <in.ymt> <out.ymt>\n  rk occlusion scan <out dir> <rpf>...\n"
                    "  rk rpf rm <rpf> <entry path>\n  rk rpf rewrite <in.rpf> <out.rpf> [--aes] [--nested]\n"
                    "  rk world info | at <x> <y> <z> | near <x> <y> <z> <radius> | occluders <x> <y> <z> <radius> | mlos [name]   [--all-lods] [--groups] [--sp] [--rebuild]\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
