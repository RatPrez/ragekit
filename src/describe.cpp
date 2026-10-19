#include "ragekit/describe.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <map>
#include "ragekit/ragekit.h"

namespace rk {

Fact& Fact::add(std::string l, std::string v) {
    children.push_back(Fact{std::move(l), std::move(v), {}});
    return children.back();
}

namespace {

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

struct Ctx {
    DescribeOptions opt;
    /// Whether item i of a list is shown; at the first one left out, adds a fact saying how many are.
    bool show(Fact& list, size_t i, size_t total) const {
        if (opt.verbose || i < opt.max_items) return true;
        if (i == opt.max_items) list.add("...", fmt("%zu more (verbose lists all)", total - i));
        return false;
    }
};

std::string vec3(const Vec3& v) { return fmt("(%.3f %.3f %.3f)", v.x, v.y, v.z); }

void lines(Fact& f, const std::string& text) {
    size_t at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        if (nl > at) f.add("", text.substr(at, nl - at));
        at = nl + 1;
    }
}

void resource_header(Fact& root, std::span<const uint8_t> file, const std::string& ext) {
    const rsc::Resource res = rsc::Resource::load(file);
    Fact& h = root.add("RSC7", fmt("version %d (%s)", res.version, game_name(rsc::game_of(ext, res.version))));
    h.add("system", fmt("%zu bytes, flags 0x%08X, %u pages, base %u", res.system.size(), res.system_flags.value, res.system_flags.page_count(),
                        res.system_flags.base_size()));
    h.add("graphics", fmt("%zu bytes, flags 0x%08X, %u pages, base %u", res.graphics.size(), res.graphics_flags.value, res.graphics_flags.page_count(),
                          res.graphics_flags.base_size()));
}

void texture(Fact& f, const Texture& t) {
    f.add(t.name, fmt("%ux%u %s, %u levels, usage %u, flags 0x%06X, unk32 0x%02X, %zu bytes", t.width, t.height, texture_format_name(t.format), t.levels,
                      (unsigned)t.usage(), t.usage_flags(), t.unk32, t.data.size()));
}

void textures(Fact& f, const Ctx& c, const TextureDictionary& txd) {
    Fact& l = f.add("textures", std::to_string(txd.textures.size()));
    for (size_t i = 0; i < txd.textures.size(); i++) if (c.show(l, i, txd.textures.size())) texture(l, txd.textures[i]);
}

void drawable(Fact& f, const Ctx& c, const Drawable& d) {
    f.add("box", vec3(d.bbox_min) + " .. " + vec3(d.bbox_max));
    f.add("sphere", vec3(d.bounding_center) + fmt(" r %.3f", d.bounding_radius));
    f.add("lod distances", fmt("%.1f %.1f %.1f %.1f", d.lod_distance[0], d.lod_distance[1], d.lod_distance[2], d.lod_distance[3]));
    f.add("render masks / flags", fmt("%08X %08X %08X %08X", d.render_mask_flags[0], d.render_mask_flags[1], d.render_mask_flags[2], d.render_mask_flags[3]));
    if (d.shader_group) {
        const ShaderGroup& sg = *d.shader_group;
        Fact& g = f.add("shader group", fmt("%zu shaders, %zu embedded textures", sg.shaders.size(), sg.textures ? sg.textures->textures.size() : 0));
        if (sg.textures) textures(g, c, *sg.textures);
        for (size_t i = 0; i < sg.shaders.size(); i++) {
            if (!c.show(g, i, sg.shaders.size())) break;
            const Shader& s = sg.shaders[i];
            Fact& sf = g.add(fmt("shader %zu", i), fmt("%s (%s), bucket %u, %zu parameters", hash_name(s.name_hash).c_str(), hash_name(s.sps_hash).c_str(),
                                                     s.render_bucket, s.params.size()));
            if (!c.opt.verbose) continue;
            for (const ShaderParam& p : s.params) {
                if (p.type == ParamType::Texture) { sf.add(hash_name(p.name_hash), fmt("texture '%s' (slot %d)", p.texture.c_str(), (int)p.slot)); continue; }
                std::string v;
                for (const Vec4& x : p.values) v += fmt("(%g %g %g %g) ", x.x, x.y, x.z, x.w);
                sf.add(hash_name(p.name_hash), v + fmt("(slot %d)", (int)p.slot));
            }
        }
    }
    if (d.skeleton) {
        Fact& s = f.add("skeleton", fmt("%zu bones", d.skeleton->bones.size()));
        if (c.opt.verbose)
            for (const Bone& b : d.skeleton->bones)
                s.add(fmt("bone %d", b.index), fmt("%s, tag %u, parent %d, sibling %d, flags 0x%04X, at %s", b.name.c_str(), b.tag, b.parent, b.next_sibling,
                                                  b.flags, vec3(b.translation).c_str()));
    }
    if (d.joints) f.add("joints", fmt("%zu rotation limits, %zu translation limits", d.joints->rotation_limits.size() / 0xC0, d.joints->translation_limits.size() / 0x40));
    static const char* lodnames[] = {"high", "medium", "low", "very low"};
    for (int l = 0; l < 4; l++) {
        if (d.lods[l].empty()) continue;
        Fact& lf = f.add(std::string("lod ") + lodnames[l], fmt("%zu models", d.lods[l].size()));
        for (const Model& m : d.lods[l]) {
            Fact& mf = lf.add("model", fmt("bone %u, skinned %d, mask 0x%02X, flags 0x%02X, %zu geometries", m.bone_index(), m.has_skin(), m.render_mask, m.flags,
                                           m.geometries.size()));
            if (c.opt.verbose)
                for (const Geometry& g : m.geometries)
                    mf.add("geometry", fmt("shader %u, %u vertices (stride %u, flags 0x%04X, types %016llX), %zu indices, %zu bone ids", g.shader_index, g.vertex_count,
                                           g.layout.stride(), g.layout.flags, (unsigned long long)g.layout.types, g.indices.size(), g.bone_ids.size()));
        }
    }
    if (!d.lods_extra.empty()) f.add("extra model list", fmt("%zu models", d.lods_extra.size()));
    if (!d.lights.empty()) f.add("lights", std::to_string(d.lights.size()));
    if (d.bound) f.add("embedded bound", bound_type_name(d.bound->type));
}

void bound(Fact& parent, const Ctx& c, const Bound& b, size_t depth = 0) {
    std::string v = fmt("material %u %s, room %u, box %s .. %s, margin %.3f", b.material_index, material_name(b.material_index), b.room_id(), vec3(b.box_min).c_str(),
                        vec3(b.box_max).c_str(), b.margin);
    if (b.geometry) {
        const auto& g = *b.geometry;
        size_t tris = 0;
        for (const BoundPolygon& p : g.polygons) if (p.kind == BoundPolygon::Triangle) tris++;
        v += fmt(", %zu vertices, %zu polygons (%zu triangles), %zu materials%s%s", g.vertices.size(), g.polygons.size(), tris, g.materials.size(),
                 g.bvh ? ", bvh" : "", g.has_octants ? ", octants" : "");
        if (g.bvh) v += fmt(" (%u / %zu nodes, %zu trees)", g.bvh->node_count, g.bvh->nodes.size(), g.bvh->trees.size());
    }
    if (b.type == BoundType::Composite) v += fmt(", %zu children%s%s", b.children.size(), b.composite_bvh ? ", bvh" : "", b.has_child_flags ? "" : ", no flags");
    Fact& f = parent.add(bound_type_name(b.type), v);
    for (size_t i = 0; i < b.children.size(); i++) {
        if (!c.show(f, i, b.children.size())) break;
        if (b.children[i].bound) bound(f, c, *b.children[i].bound, depth + 1);
        else f.add("(null child)");
    }
}

void fragment(Fact& f, const Ctx& c, const Fragment& fr) {
    f.add("sphere", vec3(fr.sphere_center) + fmt(" r %.3f", fr.sphere_radius));
    f.add("gravity / buoyancy", fmt("%.2f / %.2f", fr.gravity_factor, fr.buoyancy_factor));
    f.add("glass windows", std::to_string(fr.glass_windows.size()));
    f.add("cloths", std::to_string(fr.cloths.size()));
    f.add("lights", std::to_string(fr.lights.size()));
    if (fr.drawable) {
        Fact& d = f.add("main drawable", fr.drawable->name);
        drawable(d, c, *fr.drawable);
        if (fr.drawable->bound) bound(d, c, *fr.drawable->bound);
    }
    if (fr.drawable_cloth) drawable(f.add("cloth drawable", fr.drawable_cloth->name), c, *fr.drawable_cloth);
    for (size_t i = 0; i < fr.drawable_array.size(); i++) drawable(f.add(fmt("drawable %zu", i), fr.drawable_array_names[i]), c, fr.drawable_array[i]);
    f.add("bone transforms", std::to_string(fr.bone_transforms.size()));
    for (int l = 0; l < 3; l++) {
        if (!fr.physics[l]) continue;
        const FragPhysicsLod& lod = *fr.physics[l];
        Fact& lf = f.add(fmt("physics lod %d", l + 1), fmt("%zu groups, %zu children, %zu transforms, archetype '%s'%s", lod.groups.size(), lod.children.size(),
                                                         lod.transforms.size(), lod.archetype ? lod.archetype->name.c_str() : "", lod.archetype_damaged ? " (+damaged)" : ""));
        for (size_t i = 0; i < lod.groups.size(); i++) {
            if (!c.show(lf, i, lod.groups.size())) break;
            const FragGroup& g = lod.groups[i];
            lf.add(fmt("group %zu", i), fmt("%s, parent %u, children %u at %u, glass %u / %u, mass %.2f", g.name().c_str(), g.parent_index(), g.child_count(), g.child_index(),
                                           g.glass_window_index(), g.glass_flags(), g.mass()));
        }
        for (size_t i = 0; i < lod.children.size(); i++) {
            if (!c.show(lf, i, lod.children.size())) break;
            const FragChild& ch = lod.children[i];
            Fact& cf = lf.add(fmt("child %zu", i), fmt("group %u, bone %u, mass %.2f / %.2f%s%s", ch.group_index, ch.bone_tag, ch.pristine_mass, ch.damaged_mass,
                                                     ch.drawable ? ", drawable" : "", ch.drawable_damaged ? ", damaged drawable" : ""));
            if (c.opt.verbose && ch.drawable) drawable(cf, Ctx{DescribeOptions{false, c.opt.max_items}}, *ch.drawable);
        }
        if (lod.bound) bound(lf, c, *lod.bound);
    }
}

void meta(Fact& f, const Ctx& c, const Meta& m) {
    f.add("name", m.name);
    f.add("root block", std::to_string(m.root_block));
    Fact& s = f.add("structure infos", std::to_string(m.structs.size()));
    for (const auto& st : m.structs) {
        Fact& sf = s.add(hash_name(st.name), fmt("size %d, key %u, unk8 %u, %zu entries", st.size, st.key, st.unknown8, st.entries.size()));
        if (c.opt.verbose)
            for (const auto& e : st.entries)
                sf.add(fmt("+%d", e.offset), fmt("%s, type 0x%02X, unk9 %u, ref %d, key %s", hash_name(e.name).c_str(), e.type, e.unknown9, e.ref_index, hash_name(e.ref_key).c_str()));
    }
    Fact& e = f.add("enum infos", std::to_string(m.enums.size()));
    for (const auto& en : m.enums) e.add(hash_name(en.name), fmt("%zu values", en.entries.size()));
    Fact& b = f.add("blocks", std::to_string(m.blocks.size()));
    for (size_t i = 0; i < m.blocks.size(); i++)
        if (c.show(b, i, m.blocks.size())) b.add(fmt("block %zu", i + 1), fmt("%s, %zu bytes", hash_name(m.blocks[i].type).c_str(), m.blocks[i].data.size()));
}

Fact resource(std::span<const uint8_t> file, const std::string& ext, const Ctx& c) {
    Fact root;
    resource_header(root, file, ext);
    if (ext == ".ytd") {
        const TextureDictionary txd = read_ytd(file);
        root.label = "texture dictionary";
        root.value = fmt("%zu textures", txd.textures.size());
        textures(root, c, txd);
    } else if (ext == ".ydr") {
        const Drawable d = read_ydr(file);
        root.label = "drawable";
        root.value = d.name;
        drawable(root, c, d);
    } else if (ext == ".ydd") {
        const DrawableDictionary dd = read_ydd(file);
        root.label = "drawable dictionary";
        root.value = fmt("%zu drawables", dd.drawables.size());
        for (size_t i = 0; i < dd.drawables.size(); i++) drawable(root.add(hash_name(dd.hashes[i]), dd.drawables[i].name), c, dd.drawables[i]);
    } else if (ext == ".ybn") {
        root.label = "collision";
        if (auto b = read_ybn(file)) bound(root, c, *b);
    } else if (ext == ".yft") {
        const Fragment fr = read_yft(file);
        root.label = "fragment";
        root.value = fr.name;
        fragment(root, c, fr);
    } else if (ext == ".ynv") {
        const NavMesh n = read_ynv(file);
        root.label = "navmesh";
        root.value = fmt("area %u (cell %u, %u)", n.area_id, n.area_id % 100, n.area_id / 100);
        root.add("flags", fmt("0x%X", n.content_flags));
        root.add("contents", fmt("%zu polygons, %zu vertices, %zu points, %zu portals, %zu adjacent areas", n.polys.size(), n.vertices.size(), n.points().size(),
                                 n.portals.size(), n.adjacent_areas.size()));
        root.add("box", vec3(n.box_min()) + " .. " + vec3(n.box_max()));
    } else if (ext == ".ypt") {
        const ParticleDictionary p = read_ypt(file);
        root.label = "particle dictionary";
        root.value = p.name;
        if (p.textures) textures(root, c, *p.textures);
        root.add("drawables", std::to_string(p.drawables.size()));
        const std::pair<const char*, const PtxRuleList*> lists[] = {{"particle rules", &p.particle_rules}, {"effect rules", &p.effect_rules}, {"emitter rules", &p.emitter_rules}};
        for (auto& [kind, l] : lists) {
            Fact& lf = root.add(kind, std::to_string(l->rules.size()));
            for (size_t i = 0; i < l->rules.size(); i++) if (c.show(lf, i, l->rules.size())) lf.add("", l->name(i));
        }
        root.add("blocks", std::to_string(p.blocks().size()));
    } else if (ext == ".ytyp" || ext == ".ymap" || ext == ".ymt" || ext == ".ymf" || ext == ".ypdb") {
        const Meta m = read_meta(file);
        root.label = "meta";
        meta(root, c, m);
    } else if (ext == ".yld") {
        const ClothDictionary d = read_yld(file);
        root.label = "cloth dictionary";
        root.value = fmt("%zu cloths", d.cloths.size());
        for (size_t i = 0; i < d.cloths.size(); i++) if (c.show(root, i, d.cloths.size())) root.add("cloth", hash_name(d.hashes[i]));
    } else if (ext == ".yvr") {
        const VehicleRecording v = read_yvr(file);
        root.label = "vehicle recording";
        root.value = fmt("%zu entries, %.2f s", v.entries.size(), v.entries.empty() ? 0.0 : v.entries.back().time / 1000.0);
        for (size_t i = 0; i < v.entries.size(); i++) {
            if (!c.show(root, i, v.entries.size())) break;
            const VehicleRecordEntry& e = v.entries[i];
            root.add(fmt("%u ms", e.time), fmt("at %s, velocity %s, steer %d gas %d brake %d handbrake %u", vec3(e.position).c_str(), vec3(e.velocity_ms()).c_str(), e.steering,
                                              e.gas, e.brake, e.handbrake));
        }
    } else if (ext == ".ywr") {
        const WaypointRecording w = read_ywr(file);
        root.label = "waypoint recording";
        root.value = fmt("%zu points", w.entries.size());
        for (size_t i = 0; i < w.entries.size(); i++) {
            if (!c.show(root, i, w.entries.size())) break;
            const WaypointRecordEntry& e = w.entries[i];
            root.add(fmt("point %zu", i), fmt("%s, heading %u, speed %u, flags 0x%04X, %04X %04X", vec3(e.position).c_str(), e.heading, e.speed, e.flags, e.unk2, e.unk3));
        }
    } else if (ext == ".ynd") {
        const PathNodes n = read_ynd(file);
        root.label = "path nodes";
        root.value = fmt("%zu nodes (%u vehicle, %u ped), %zu links, %zu junctions", n.nodes.size(), n.vehicle_node_count, n.ped_node_count, n.links.size(), n.junctions.size());
        root.add("junction height maps", fmt("%zu bytes", n.heightmaps.size()));
        root.add("junction references", std::to_string(n.junction_refs.size()));
        for (size_t i = 0; i < n.nodes.size(); i++) {
            if (!c.show(root, i, n.nodes.size())) break;
            const PathNode& p = n.nodes[i];
            root.add(fmt("node %u:%u", p.area_id, p.node_id), fmt("%s, street %s, %d links from %u, flags %02X %02X %02X %02X %02X %X", vec3(p.position()).c_str(),
                                                                  hash_name(p.street_name).c_str(), p.link_count(), p.link_id, p.flags0, p.flags1, p.flags2, p.flags3,
                                                                  p.flags4, p.link_count_flags & 7));
        }
    } else if (ext == ".ycd") {
        const ClipDictionary d = read_ycd(file);
        const auto clips = d.clips();
        const auto anims = d.animations();
        root.label = "clip dictionary";
        root.value = fmt("%zu clips, %zu animations, %zu structures", clips.size(), anims.size(), graph_blocks(d.root).size());
        Fact& cl = root.add("clips", std::to_string(clips.size()));
        for (size_t i = 0; i < clips.size(); i++) {
            if (!c.show(cl, i, clips.size())) break;
            const ClipRef& x = clips[i];
            cl.add(x.name(), fmt("%s, %.3f .. %.3f s, rate %.2f, %zu animations, %zu properties, %zu tags", x.type() == ClipType::Animation ? "animation" : "list",
                                 x.start_time(), x.end_time(), x.rate(), x.animations().size(), x.property_names().size(), x.tag_count()));
        }
        Fact& an = root.add("animations", std::to_string(anims.size()));
        for (size_t i = 0; i < anims.size(); i++) {
            if (!c.show(an, i, anims.size())) break;
            const AnimationRef& a = anims[i];
            size_t bytes = 0;
            for (const RscBlockPtr& s : a.sequences()) bytes += s->bytes.size();
            const auto ids = a.bone_ids();
            Fact& af = an.add(hash_name(a.hash), fmt("%u frames, %.3f s, %zu sequences (%zu bytes), %zu tracks", a.frames(), a.duration(), a.sequences().size(), bytes, ids.size()));
            if (c.opt.verbose)
                for (const AnimationBoneId& id : ids) af.add(fmt("bone %u", id.bone), fmt("track %u, format %u", id.track, id.format));
        }
    } else if (ext == ".yed") {
        const auto ex = read_yed(file).expressions();
        root.label = "expression dictionary";
        root.value = fmt("%zu expressions", ex.size());
        for (size_t i = 0; i < ex.size(); i++) {
            if (!c.show(root, i, ex.size())) break;
            const ExpressionRef& e = ex[i];
            Fact& ef = root.add(e.name(), fmt("%zu streams, %zu tracks, %zu springs, %zu variables, signature 0x%08X", e.stream_count(), e.tracks().size(), e.spring_count(),
                                              e.variables().size(), e.signature()));
            if (c.opt.verbose)
                for (const ExpressionTrack& t : e.tracks()) ef.add(fmt("bone %u", t.bone), fmt("track %u, format %u, flags 0x%02X", t.track, t.flags & 0x7F, t.flags & 0x80));
        }
    } else if (ext == ".ysc") {
        const Script s = read_ysc(file);
        root.label = "script";
        root.value = s.name();
        root.add("code", fmt("%u bytes", s.code_size()));
        root.add("parameters / statics", fmt("%u / %u", s.parameter_count(), s.static_count()));
        if (s.global_count()) root.add("globals", fmt("%u in block %u", s.global_count(), s.global_block()));
        root.add("globals signature", fmt("0x%08X", s.globals_signature()));
        root.add("strings", fmt("%u bytes", s.string_size()));
        Fact& n = root.add("natives", std::to_string(s.native_count()));
        const auto nat = s.natives();
        for (size_t i = 0; i < nat.size(); i++) if (c.show(n, i, nat.size())) n.add(std::to_string(i), fmt("0x%016llX", (unsigned long long)nat[i]));
    } else if (ext == ".ybd") {
        const BoundDictionary d = read_ybd(file);
        root.label = "bound dictionary";
        root.value = fmt("%zu bounds", d.bounds.size());
        for (size_t i = 0; i < d.bounds.size(); i++)
            if (c.show(root, i, d.bounds.size()) && d.bounds[i]) bound(root.add(hash_name(d.hashes[i])), c, *d.bounds[i]);
    } else if (ext == ".yfd") {
        const auto fl = read_yfd(file).filters();
        root.label = "frame filter dictionary";
        root.value = fmt("%zu filters", fl.size());
        for (size_t i = 0; i < fl.size(); i++)
            if (c.show(root, i, fl.size())) root.add(hash_name(fl[i].hash), fmt("type %u, %zu entries, %zu weights, signature 0x%08X", fl[i].type(), fl[i].entry_count(),
                                                                                 fl[i].weights().size(), fl[i].signature()));
    } else {
        root.label = "resource";
        root.value = "no reader for " + ext;
    }
    return root;
}

}  // namespace

Fact describe(std::span<const uint8_t> file, const std::string& name, const DescribeOptions& opt) {
    const Ctx c{opt};
    const std::string ext = lower(std::filesystem::path(name).extension().string());
    Fact root;
    if (is_rbf(file)) {
        root = {"RBF", fmt("%zu bytes", file.size()), {}};
        lines(root, rbf_dump(read_rbf(file)));
    } else if (is_pso(file)) {
        const Pso p = read_pso(file);
        root = {"PSO", fmt("%zu bytes", file.size()), {}};
        lines(root, pso_describe(p, opt.verbose));
        if (p.has_map && p.has_schema) {
            PsoView v(p);
            const PsoObject r = v.root();
            root.add("root", r ? fmt("%s, %zu fields", pso_name(r.type).c_str(), v.fields(r).size()) : std::string("(none)"));
        }
    } else if (is_gxt2(file)) {
        const TextTable t = read_gxt2(file);
        root = {"text table", fmt("%zu entries", t.entries.size()), {}};
        for (size_t i = 0; i < t.entries.size(); i++) if (c.show(root, i, t.entries.size())) root.add(hash_name(t.entries[i].hash), t.entries[i].text);
    } else if (ext == ".awc" && is_awc(file)) {
        const AwcFile a = read_awc(file);
        root = {"audio container", fmt("version %u, flags 0x%04X%s, %zu streams", a.version, a.flags, a.whole_file_encrypted ? ", encrypted" : "", a.streams.size()), {}};
        Fact& st = root.add("streams", std::to_string(a.streams.size()));
        for (const AwcStream& s : a.streams) {
            std::string v;
            for (const AwcChunk& ch : s.chunks) v += fmt("%02X@%u+%zu ", ch.type, ch.offset, ch.data.size());
            st.add(hash_name(s.id), v);
        }
        Fact& so = root.add("sounds");
        for (const AwcAudio& s : awc_decode(a))
            so.add(hash_name(s.id), fmt("%u samples at %u Hz (%.2f s), %s", s.samples, s.sample_rate, s.sample_rate ? (double)s.samples / s.sample_rate : 0.0,
                                        s.codec == 4 ? "ADPCM" : s.codec == 0 ? "PCM" : fmt("codec %u, not decoded", s.codec).c_str()));
    } else if ((ext == ".ugc" || ext == ".json") && [&] {
                   size_t i = file.size() >= 3 && file[0] == 0xEF ? 3 : 0;
                   while (i < file.size() && std::isspace(file[i])) i++;
                   return i < file.size() && (file[i] == '{' || file[i] == '[');
               }()) {
        const JsonValue j = parse_json(std::string_view((const char*)file.data(), file.size()));
        root = {"JSON", j.type == JsonValue::Type::Object ? fmt("an object of %zu members", j.members.size()) : fmt("a value of type %d", (int)j.type), {}};
        for (size_t i = 0; i < j.members.size(); i++) {
            if (!c.show(root, i, j.members.size())) break;
            const JsonValue& m = j.members[i].second;
            root.add(j.members[i].first, m.type == JsonValue::Type::Object ? fmt("%zu members", m.members.size()) : m.type == JsonValue::Type::Array ? fmt("%zu items", m.items.size())
                                                                                                                    : m.type == JsonValue::Type::Bool ? (m.boolean ? "true" : "false") : m.text);
        }
    } else if (is_gfx(file)) {
        const GfxMovie m = read_gfx(file);
        root = {"Scaleform movie", fmt("version %u%s, %zu tags, %u frames at %.1f fps", m.version, m.compressed ? ", compressed" : "", m.tags.size(), m.frame_count(), m.frame_rate()), {}};
        std::map<uint16_t, size_t> codes;
        for (const GfxTag& t : m.tags) codes[t.code]++;
        Fact& tf = root.add("tags by code");
        for (auto& [code, n] : codes) tf.add(std::to_string(code), std::to_string(n));
        const auto ex = m.exports();
        Fact& ef = root.add("exports", std::to_string(ex.size()));
        for (size_t i = 0; i < ex.size(); i++) if (c.show(ef, i, ex.size())) ef.add("", ex[i]);
    } else if (ext == ".nametable") {
        const auto names = read_nametable(file);
        root = {"name table", fmt("%zu names", names.size()), {}};
        for (size_t i = 0; i < names.size(); i++) if (c.show(root, i, names.size())) root.add(fmt("0x%08X", joaat_lower(names[i])), names[i]);
    } else if (ext == ".fxc") {
        const FxcFile f = read_fxc(file);
        root = {"compiled shader effect", fmt("vertex type %u", f.vertex_type), {}};
        Fact& cb = root.add("constant buffers", std::to_string(f.cbuffers.size()));
        for (const auto& b : f.cbuffers) cb.add(b.name, fmt("%u bytes, vs %u ps %u", b.size, b.slot[0], b.slot[1]));
        Fact& vars = root.add("variables", std::to_string(f.variables.size()));
        for (size_t i = 0; i < f.variables.size(); i++)
            if (c.show(vars, i, f.variables.size())) vars.add(f.variables[i].name, fmt("type %u, slot %u, offset %u", (unsigned)f.variables[i].type, f.variables[i].slot, f.variables[i].offset));
        Fact& te = root.add("techniques", std::to_string(f.techniques.size()));
        for (const auto& t : f.techniques) te.add(t.name, fmt("%zu passes", t.passes.size()));
    } else if (ext == ".rpf") {
        const Rpf r = Rpf::open_memory(Bytes(file.begin(), file.end()), std::filesystem::path(name).filename().string());
        size_t dirs = 0, bins = 0, res = 0, nested = 0;
        for (const RpfEntry& e : r.entries()) (e.is_directory() ? dirs : e.is_resource() ? res : e.is_archive() ? nested : bins)++;
        const char* enc = r.encryption() == RpfEncryption::None ? "none" : r.encryption() == RpfEncryption::Open ? "OPEN" : r.encryption() == RpfEncryption::Aes ? "AES" : "NG";
        root = {"archive", fmt("%zu entries, encryption %s", r.entries().size(), enc), {}};
        root.add("contents", fmt("%zu directories, %zu binary files, %zu resources, %zu nested archives", dirs, bins, res, nested));
    } else if (rsc::Resource::is_resource(file)) {
        root = resource(file, ext, c);
    } else if ((ext == ".meta" || ext == ".xml") && !file.empty()) {
        const XmlElement x = parse_xml(std::string_view((const char*)file.data(), file.size()));
        root = {"XML", fmt("<%s>, %zu children", x.name.c_str(), x.children.size()), {}};
    } else {
        root = {"file", fmt("%zu bytes, no reader for '%s'", file.size(), ext.c_str()), {}};
    }
    return root;
}

std::string format_facts(const Fact& root) {
    std::string out;
    std::vector<std::pair<const Fact*, size_t>> stack{{&root, 0}};
    while (!stack.empty()) {
        auto [f, depth] = stack.back();
        stack.pop_back();
        out.append(depth * 2, ' ');
        if (f->label.empty()) out += f->value;
        else if (f->value.empty()) out += f->label;
        else out += f->label + ": " + f->value;
        out += '\n';
        for (auto it = f->children.rbegin(); it != f->children.rend(); ++it) stack.push_back({&*it, depth + 1});
    }
    return out;
}

}  // namespace rk
