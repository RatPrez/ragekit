// rk_roundtrip: reads every resource under the given paths (or a sample of a game install, --game), writes it back, reads
// the result, and compares the two models field by field. Prints one line per file. --record / --check keep a digest of
// everything written per file, so a rewrite can be held to the bytes the previous code wrote.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include "ragekit/gameindex.h"
#include "ragekit/manifest.h"
#include "ragekit/ragekit.h"

namespace fs = std::filesystem;
using namespace rk;

static uint64_t g_digest = 0;
static void mix(const Bytes& b) {
    uint64_t h = g_digest ? g_digest : 1469598103934665603ull;
    for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
    g_digest = h ^ b.size();
}

static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

struct Cmp {
    std::string first_diff;
    int diffs = 0;
    void fail(const std::string& what) { if (first_diff.empty()) first_diff = what; diffs++; }
    template <class T> void eq(const T& a, const T& b, const std::string& what) { if (!(a == b)) fail(what); }
    void eqf(float a, float b, const std::string& what) { if (std::memcmp(&a, &b, 4) != 0) fail(what); }
    void eqv(const Vec3& a, const Vec3& b, const std::string& w) { eqf(a.x, b.x, w); eqf(a.y, b.y, w); eqf(a.z, b.z, w); }
    void eqv(const Vec4& a, const Vec4& b, const std::string& w) { eqf(a.x, b.x, w); eqf(a.y, b.y, w); eqf(a.z, b.z, w); eqf(a.w, b.w, w); }
};

static void compare_texture(Cmp& c, const Texture& a, const Texture& b, const std::string& p) {
    c.eq(a.name, b.name, p + ".name");
    c.eq(a.width, b.width, p + ".width");
    c.eq(a.height, b.height, p + ".height");
    c.eq(a.format, b.format, p + ".format");
    c.eq(a.levels, b.levels, p + ".levels");
    c.eq(a.usage_data, b.usage_data, p + ".usage");
    c.eq(a.extra_flags, b.extra_flags, p + ".extra_flags");
    c.eq(a.unk32, b.unk32, p + ".unk32");
    c.eq(a.data, b.data, p + ".data");
}

static void compare_txd(Cmp& c, const TextureDictionary& a, const TextureDictionary& b, const std::string& p) {
    c.eq(a.textures.size(), b.textures.size(), p + ".count");
    for (size_t i = 0; i < a.textures.size() && i < b.textures.size(); i++) compare_texture(c, a.textures[i], b.textures[i], p + "[" + a.textures[i].name + "]");
}

static void compare_drawable(Cmp& c, const Drawable& a, const Drawable& b, const std::string& p) {
    c.eq(a.name, b.name, p + ".name");
    c.eqv(a.bbox_min, b.bbox_min, p + ".bbox_min");
    c.eqv(a.bbox_max, b.bbox_max, p + ".bbox_max");
    c.eqv(a.bounding_center, b.bounding_center, p + ".center");
    c.eqf(a.bounding_radius, b.bounding_radius, p + ".radius");
    for (int i = 0; i < 4; i++) { c.eqf(a.lod_distance[i], b.lod_distance[i], p + ".lod_distance"); c.eq(a.render_mask_flags[i], b.render_mask_flags[i], p + ".render_mask_flags"); }
    c.eq(a.shader_group.has_value(), b.shader_group.has_value(), p + ".has_shader_group");
    if (a.shader_group && b.shader_group) {
        auto& sa = *a.shader_group; auto& sb = *b.shader_group;
        c.eq(sa.textures.has_value(), sb.textures.has_value(), p + ".has_textures");
        if (sa.textures && sb.textures) compare_txd(c, *sa.textures, *sb.textures, p + ".txd");
        c.eq(sa.shaders.size(), sb.shaders.size(), p + ".shaders.count");
        for (size_t i = 0; i < sa.shaders.size() && i < sb.shaders.size(); i++) {
            auto& x = sa.shaders[i]; auto& y = sb.shaders[i];
            std::string sp = p + ".shader[" + std::to_string(i) + "]";
            c.eq(x.name_hash, y.name_hash, sp + ".name"); c.eq(x.sps_hash, y.sps_hash, sp + ".sps");
            c.eq(x.render_bucket, y.render_bucket, sp + ".bucket"); c.eq(x.unk12, y.unk12, sp + ".unk12");
            c.eq(x.params.size(), y.params.size(), sp + ".params.count");
            for (size_t k = 0; k < x.params.size() && k < y.params.size(); k++) {
                auto& u = x.params[k]; auto& v = y.params[k];
                std::string pp = sp + ".param[" + hash_name(u.name_hash) + "]";
                c.eq(u.name_hash, v.name_hash, pp + ".name"); c.eq(u.type, v.type, pp + ".type");
                c.eq(u.texture, v.texture, pp + ".texture");
                c.eq(u.slot, v.slot, pp + ".slot");   // read slots are written back as they were (-1 = compute is only for new parameters)
                c.eq(u.values.size(), v.values.size(), pp + ".values.count");
                for (size_t m = 0; m < u.values.size() && m < v.values.size(); m++) c.eqv(u.values[m], v.values[m], pp + ".value");
            }
        }
    }
    c.eq(a.skeleton.has_value(), b.skeleton.has_value(), p + ".has_skeleton");
    if (a.skeleton && b.skeleton) {
        auto& x = *a.skeleton; auto& y = *b.skeleton;
        c.eq(x.bones.size(), y.bones.size(), p + ".bones.count");
        c.eq(x.unk1C, y.unk1C, p + ".skel.unk1C"); c.eq(x.unk50, y.unk50, p + ".skel.unk50"); c.eq(x.unk54, y.unk54, p + ".skel.unk54"); c.eq(x.unk58, y.unk58, p + ".skel.unk58");
        for (size_t i = 0; i < x.bones.size() && i < y.bones.size(); i++) {
            auto& u = x.bones[i]; auto& v = y.bones[i];
            std::string bp = p + ".bone[" + u.name + "]";
            c.eq(u.name, v.name, bp + ".name"); c.eq(u.tag, v.tag, bp + ".tag"); c.eq(u.index, v.index, bp + ".index");
            c.eq(u.parent, v.parent, bp + ".parent"); c.eq(u.next_sibling, v.next_sibling, bp + ".sibling"); c.eq(u.flags, v.flags, bp + ".flags");
            c.eqv(u.translation, v.translation, bp + ".translation"); c.eqv(u.scale, v.scale, bp + ".scale");
            c.eqf(u.rotation.x, v.rotation.x, bp + ".rot"); c.eqf(u.rotation.y, v.rotation.y, bp + ".rot"); c.eqf(u.rotation.z, v.rotation.z, bp + ".rot"); c.eqf(u.rotation.w, v.rotation.w, bp + ".rot");
        }
        c.eq(x.parent_indices, y.parent_indices, p + ".skel.parents");
        c.eq(x.child_indices, y.child_indices, p + ".skel.children");
        c.eq(x.transforms.size(), y.transforms.size(), p + ".skel.transforms.count");
        for (size_t i = 0; i < x.transforms.size() && i < y.transforms.size(); i++)
            for (int k = 0; k < 16; k++) { c.eqf(x.transforms[i].m[k], y.transforms[i].m[k], p + ".skel.transform"); c.eqf(x.transforms_inverse[i].m[k], y.transforms_inverse[i].m[k], p + ".skel.transform_inv"); }
    }
    c.eq(a.joints.has_value(), b.joints.has_value(), p + ".has_joints");
    if (a.joints && b.joints) { c.eq(a.joints->rotation_limits, b.joints->rotation_limits, p + ".joints.rot"); c.eq(a.joints->translation_limits, b.joints->translation_limits, p + ".joints.trans"); }
    for (int l = 0; l < 4; l++) {
        auto& la = a.lods[l]; auto& lb = b.lods[l];
        std::string lp = p + ".lod" + std::to_string(l);
        c.eq(la.size(), lb.size(), lp + ".models.count");
        for (size_t i = 0; i < la.size() && i < lb.size(); i++) {
            auto& x = la[i]; auto& y = lb[i];
            std::string mp = lp + ".model[" + std::to_string(i) + "]";
            c.eq(x.skeleton_binding, y.skeleton_binding, mp + ".binding"); c.eq(x.render_mask, y.render_mask, mp + ".mask"); c.eq(x.flags, y.flags, mp + ".flags");
            c.eqv(x.bounds.min, y.bounds.min, mp + ".bounds"); c.eqv(x.bounds.max, y.bounds.max, mp + ".bounds");
            c.eq(x.geometries.size(), y.geometries.size(), mp + ".geoms.count");
            for (size_t k = 0; k < x.geometries.size() && k < y.geometries.size(); k++) {
                auto& u = x.geometries[k]; auto& v = y.geometries[k];
                std::string gp = mp + ".geom[" + std::to_string(k) + "]";
                c.eq(u.layout.flags, v.layout.flags, gp + ".layout.flags"); c.eq(u.layout.types, v.layout.types, gp + ".layout.types");
                c.eq(u.vertex_count, v.vertex_count, gp + ".vertex_count"); c.eq(u.vertices, v.vertices, gp + ".vertices");
                c.eq(u.indices, v.indices, gp + ".indices"); c.eq(u.shader_index, v.shader_index, gp + ".shader");
                c.eq(u.bone_ids, v.bone_ids, gp + ".bone_ids"); c.eq(u.vb_flags, v.vb_flags, gp + ".vb_flags");
                c.eqv(u.bounds.min, v.bounds.min, gp + ".bounds"); c.eqv(u.bounds.max, v.bounds.max, gp + ".bounds");
            }
        }
    }
    c.eq(a.lods_extra.size(), b.lods_extra.size(), p + ".extra.count");
    c.eq(a.lights.size(), b.lights.size(), p + ".lights.count");
    for (size_t i = 0; i < a.lights.size() && i < b.lights.size(); i++) {
        auto& x = a.lights[i]; auto& y = b.lights[i];
        std::string lp = p + ".light[" + std::to_string(i) + "]";
        c.eqv(x.position, y.position, lp + ".position"); c.eqf(x.intensity, y.intensity, lp + ".intensity"); c.eq(x.flags, y.flags, lp + ".flags");
        c.eq(x.type, y.type, lp + ".type"); c.eqf(x.falloff, y.falloff, lp + ".falloff"); c.eqv(x.direction, y.direction, lp + ".direction");
        c.eq(x.projected_texture_hash, y.projected_texture_hash, lp + ".tex"); c.eq(x.colour[0], y.colour[0], lp + ".colour");
    }
}

static void compare_bound(Cmp& c, const Bound& a, const Bound& b, const std::string& p) {
    c.eq(a.type, b.type, p + ".type");
    c.eqv(a.box_min, b.box_min, p + ".box_min"); c.eqv(a.box_max, b.box_max, p + ".box_max"); c.eqv(a.box_center, b.box_center, p + ".box_center");
    c.eqv(a.sphere_center, b.sphere_center, p + ".sphere_center"); c.eqf(a.sphere_radius, b.sphere_radius, p + ".sphere_radius"); c.eqf(a.margin, b.margin, p + ".margin");
    c.eq(a.material_index, b.material_index, p + ".material"); c.eq(a.room_ped_density, b.room_ped_density, p + ".room"); c.eq(a.unk_flags, b.unk_flags, p + ".unk_flags");
    c.eq(a.poly_flags, b.poly_flags, p + ".poly_flags"); c.eqf(a.volume, b.volume, p + ".volume"); c.eqv(a.inertia, b.inertia, p + ".inertia");
    c.eq(a.geometry.has_value(), b.geometry.has_value(), p + ".has_geometry");
    if (a.geometry && b.geometry) {
        auto& x = *a.geometry; auto& y = *b.geometry;
        c.eq(x.vertices.size(), y.vertices.size(), p + ".verts.count");
        for (size_t i = 0; i < x.vertices.size() && i < y.vertices.size(); i++) c.eqv(x.vertices[i], y.vertices[i], p + ".vertex");
        c.eq(x.vertices_shrunk.size(), y.vertices_shrunk.size(), p + ".shrunk.count");
        for (size_t i = 0; i < x.vertices_shrunk.size() && i < y.vertices_shrunk.size(); i++) c.eqv(x.vertices_shrunk[i], y.vertices_shrunk[i], p + ".shrunk");
        c.eq(x.polygons.size(), y.polygons.size(), p + ".polys.count");
        for (size_t i = 0; i < x.polygons.size() && i < y.polygons.size(); i++) { c.eq(x.polygons[i].raw, y.polygons[i].raw, p + ".poly.raw"); c.eq(x.polygons[i].kind, y.polygons[i].kind, p + ".poly.kind"); }
        c.eq(x.vertex_colours, y.vertex_colours, p + ".vertex_colours");
        c.eq(x.materials.size(), y.materials.size(), p + ".materials.count");
        for (size_t i = 0; i < x.materials.size() && i < y.materials.size(); i++) c.eq(x.materials[i], y.materials[i], p + ".material");
        c.eq(x.material_colours, y.material_colours, p + ".material_colours"); c.eq(x.polygon_materials, y.polygon_materials, p + ".polygon_materials");
        c.eq(x.has_octants, y.has_octants, p + ".has_octants");
        for (int i = 0; i < 8; i++) c.eq(x.octants[i], y.octants[i], p + ".octant");
        c.eq(x.bvh.has_value(), y.bvh.has_value(), p + ".has_bvh");
        if (x.bvh && y.bvh) {
            c.eq(x.bvh->nodes.size(), y.bvh->nodes.size(), p + ".bvh.nodes.count"); c.eq(x.bvh->node_count, y.bvh->node_count, p + ".bvh.node_count");
            for (size_t i = 0; i < x.bvh->nodes.size() && i < y.bvh->nodes.size(); i++) if (std::memcmp(&x.bvh->nodes[i], &y.bvh->nodes[i], 16) != 0) c.fail(p + ".bvh.node");
            c.eq(x.bvh->trees.size(), y.bvh->trees.size(), p + ".bvh.trees.count");
            for (size_t i = 0; i < x.bvh->trees.size() && i < y.bvh->trees.size(); i++) if (std::memcmp(&x.bvh->trees[i], &y.bvh->trees[i], 16) != 0) c.fail(p + ".bvh.tree");
            c.eqv(x.bvh->quantum, y.bvh->quantum, p + ".bvh.quantum"); c.eqv(x.bvh->bbox_center, y.bvh->bbox_center, p + ".bvh.center");
        }
    }
    c.eq(a.children.size(), b.children.size(), p + ".children.count");
    c.eq(a.composite_bvh.has_value(), b.composite_bvh.has_value(), p + ".has_composite_bvh");
    for (size_t i = 0; i < a.children.size() && i < b.children.size(); i++) {
        auto& x = a.children[i]; auto& y = b.children[i];
        std::string cp = p + ".child[" + std::to_string(i) + "]";
        c.eq((bool)x.bound, (bool)y.bound, cp + ".present");
        if (std::memcmp(&x.transform, &y.transform, sizeof(BoundTransform)) != 0) c.fail(cp + ".transform");
        c.eqv(x.bbox.min, y.bbox.min, cp + ".bbox"); c.eqv(x.bbox.max, y.bbox.max, cp + ".bbox");
        c.eq(x.flags1, y.flags1, cp + ".flags1"); c.eq(x.flags2, y.flags2, cp + ".flags2");
        if (x.bound && y.bound) compare_bound(c, *x.bound, *y.bound, cp);
    }
}

// Generic meta layer: structure/enum infos and every data block must come back identical.
static void compare_meta(Cmp& c, const Meta& a, const Meta& b, const std::string& p) {
    c.eq(a.name, b.name, p + ".name");
    c.eq(a.root_block, b.root_block, p + ".root_block");
    c.eq(a.signature, b.signature, p + ".signature");
    c.eq(a.unknown50, b.unknown50, p + ".unknown50");
    c.eq(a.structs.size(), b.structs.size(), p + ".structs.count");
    for (size_t i = 0; i < a.structs.size() && i < b.structs.size(); i++) {
        auto& x = a.structs[i]; auto& y = b.structs[i];
        std::string sp = p + ".struct[" + std::to_string(i) + "]";
        c.eq(x.name, y.name, sp + ".name"); c.eq(x.key, y.key, sp + ".key"); c.eq(x.unknown8, y.unknown8, sp + ".unknown8");
        c.eq(x.size, y.size, sp + ".size"); c.eq(x.entries.size(), y.entries.size(), sp + ".entries.count");
        for (size_t k = 0; k < x.entries.size() && k < y.entries.size(); k++)
            if (std::memcmp(&x.entries[k], &y.entries[k], sizeof(MetaEntry)) != 0) c.fail(sp + ".entry[" + std::to_string(k) + "]");
    }
    c.eq(a.enums.size(), b.enums.size(), p + ".enums.count");
    for (size_t i = 0; i < a.enums.size() && i < b.enums.size(); i++) {
        c.eq(a.enums[i].name, b.enums[i].name, p + ".enum.name");
        c.eq(a.enums[i].entries.size(), b.enums[i].entries.size(), p + ".enum.count");
        for (size_t k = 0; k < a.enums[i].entries.size() && k < b.enums[i].entries.size(); k++)
            if (a.enums[i].entries[k].name != b.enums[i].entries[k].name || a.enums[i].entries[k].value != b.enums[i].entries[k].value) c.fail(p + ".enum.entry");
    }
    c.eq(a.blocks.size(), b.blocks.size(), p + ".blocks.count");
    for (size_t i = 0; i < a.blocks.size() && i < b.blocks.size(); i++) {
        c.eq(a.blocks[i].type, b.blocks[i].type, p + ".block[" + std::to_string(i) + "].type");
        c.eq(a.blocks[i].data == b.blocks[i].data, true, p + ".block[" + std::to_string(i) + "].bytes");
    }
}

// Bytes of a structure that are values (not pointers): marks pointer fields in `skip`.
static void value_mask(const Meta& m, const MetaStruct& s, size_t base, std::vector<bool>& skip) {
    for (auto& e : s.entries) {
        if (e.name == metatype::kArrayInfo) continue;
        size_t at = base + (size_t)e.offset;
        if (e.type == metatype::Array || e.type == metatype::CharPointer) { for (size_t k = 0; k < 8 && at + k < skip.size(); k++) skip[at + k] = true;
            for (size_t k = 10; k < 12 && at + k < skip.size(); k++) skip[at + k] = true; /* capacity */ }
        else if (e.type == metatype::DataBlockPointer || e.type == metatype::StructurePointer) { for (size_t k = 0; k < 8 && at + k < skip.size(); k++) skip[at + k] = true; }
        else if (e.type == metatype::Structure) { if (auto* sub = m.find_struct(e.ref_key)) value_mask(m, *sub, at, skip); }
    }
}
// Every structure item the file holds, in order, must come back with the same values and padding.
static void compare_struct_items(Cmp& c, const Meta& a, const Meta& b) {
    for (auto& s : a.structs) {
        if (s.size <= 0) continue;
        std::vector<bool> skip((size_t)s.size, false);
        value_mask(a, s, 0, skip);
        Bytes ia, ib;
        for (auto& blk : a.blocks) if (blk.type == s.name) ia.insert(ia.end(), blk.data.begin(), blk.data.end());
        for (auto& blk : b.blocks) if (blk.type == s.name) ib.insert(ib.end(), blk.data.begin(), blk.data.end());
        // items are padded to 16 bytes inside a block; compare item by item
        size_t stride_a = ((size_t)s.size + 15) & ~size_t(15);
        (void)stride_a;
        auto items = [&](const Meta& m) {
            std::vector<const uint8_t*> v;
            for (auto& blk : m.blocks) if (blk.type == s.name) for (size_t p = 0; p + s.size <= blk.data.size(); p += s.size) v.push_back(blk.data.data() + p);
            return v;
        };
        auto va = items(a), vb = items(b);
        // the generic reader sees arrays of structures as contiguous; item counts can differ only by 16-byte padding slack
        // Rockstar's exporter packs arrays of vec3 (12 bytes) without padding; the writer pads each array to 16 bytes like
        // CodeWalker's MetaBuilder, so the block grows by a few bytes and the item counter sees one extra item. Not a difference
        // in content when the blocks only differ by that slack.
        if (va.size() != vb.size() && ((ia.size() + 15) & ~size_t(15)) == ((ib.size() + 15) & ~size_t(15))) {
            size_t n = std::min(va.size(), vb.size()); va.resize(n); vb.resize(n);
        }
        if (va.size() != vb.size()) { c.fail("struct " + hash_name(s.name) + " item count " + std::to_string(va.size()) + " vs " + std::to_string(vb.size())); continue; }
        for (size_t i = 0; i < va.size(); i++)
            for (size_t k = 0; k < (size_t)s.size; k++)
                if (!skip[k] && va[i][k] != vb[i][k]) { c.fail("struct " + hash_name(s.name) + " item " + std::to_string(i) + " byte " + std::to_string(k)); i = va.size(); break; }
    }
}

// Typed map model: every field, and detached objects byte for byte (with their links).
static void compare_object(Cmp& c, const MetaObject& a, const MetaObject& b, const std::string& p) {
    c.eq(a.type, b.type, p + ".type");
    c.eq(a.data == b.data, true, p + ".data");
    c.eq(a.links.size(), b.links.size(), p + ".links.count");
    for (size_t i = 0; i < a.links.size() && i < b.links.size(); i++) {
        auto& x = a.links[i]; auto& y = b.links[i];
        std::string lp = p + ".link[" + std::to_string(i) + "]";
        c.eq((int)x.kind, (int)y.kind, lp + ".kind"); c.eq(x.offset, y.offset, lp + ".offset"); c.eq(x.block_type, y.block_type, lp + ".block_type");
        c.eq(x.count, y.count, lp + ".count"); c.eq(x.raw == y.raw, true, lp + ".raw"); c.eq(x.items.size(), y.items.size(), lp + ".items.count");
        for (size_t k = 0; k < x.items.size() && k < y.items.size(); k++) compare_object(c, x.items[k], y.items[k], lp + ".item");
    }
}
static void compare_objects(Cmp& c, const std::vector<MetaObject>& a, const std::vector<MetaObject>& b, const std::string& p) {
    c.eq(a.size(), b.size(), p + ".count");
    for (size_t i = 0; i < a.size() && i < b.size(); i++) compare_object(c, a[i], b[i], p + "[" + std::to_string(i) + "]");
}
static void compare_entity(Cmp& c, const Entity& a, const Entity& b, const std::string& p) {
    c.eq(a.archetype_name, b.archetype_name, p + ".archetype"); c.eq(a.flags, b.flags, p + ".flags"); c.eq(a.guid, b.guid, p + ".guid");
    c.eqv(a.position, b.position, p + ".position"); c.eqv(a.rotation, b.rotation, p + ".rotation");
    c.eqf(a.scale_xy, b.scale_xy, p + ".scale_xy"); c.eqf(a.scale_z, b.scale_z, p + ".scale_z");
    c.eq(a.parent_index, b.parent_index, p + ".parent"); c.eqf(a.lod_dist, b.lod_dist, p + ".lod_dist"); c.eqf(a.child_lod_dist, b.child_lod_dist, p + ".child_lod_dist");
    c.eq(a.lod_level, b.lod_level, p + ".lod_level"); c.eq(a.num_children, b.num_children, p + ".num_children"); c.eq(a.priority_level, b.priority_level, p + ".priority");
    c.eq(a.ambient_occlusion_multiplier, b.ambient_occlusion_multiplier, p + ".aom"); c.eq(a.artificial_ambient_occlusion, b.artificial_ambient_occlusion, p + ".aao");
    c.eq(a.tint_value, b.tint_value, p + ".tint");
    c.eq(a.is_mlo_instance, b.is_mlo_instance, p + ".is_mlo"); c.eq(a.group_id, b.group_id, p + ".group"); c.eq(a.floor_id, b.floor_id, p + ".floor");
    c.eq(a.default_entity_sets, b.default_entity_sets, p + ".default_sets"); c.eq(a.num_exit_portals, b.num_exit_portals, p + ".exit_portals");
    c.eq(a.mlo_inst_flags, b.mlo_inst_flags, p + ".mlo_flags");
    compare_objects(c, a.extensions, b.extensions, p + ".ext");
}
static void compare_entities(Cmp& c, const std::vector<Entity>& a, const std::vector<Entity>& b, const std::string& p) {
    c.eq(a.size(), b.size(), p + ".count");
    for (size_t i = 0; i < a.size() && i < b.size(); i++) compare_entity(c, a[i], b[i], p + "[" + std::to_string(i) + "]");
}
static void compare_types(Cmp& c, const MapTypes& a, const MapTypes& b) {
    c.eq(a.name, b.name, "name"); c.eq(a.dependencies, b.dependencies, "dependencies");
    compare_objects(c, a.extensions, b.extensions, "extensions");
    compare_objects(c, a.composite_entity_types, b.composite_entity_types, "composite");
    c.eq(a.archetypes.size(), b.archetypes.size(), "archetypes.count");
    for (size_t i = 0; i < a.archetypes.size() && i < b.archetypes.size(); i++) {
        auto& x = a.archetypes[i]; auto& y = b.archetypes[i];
        std::string p = "arch[" + std::to_string(i) + "]";
        c.eq((int)x.kind, (int)y.kind, p + ".kind");
        if (x.kind == Archetype::Kind::Other) { compare_object(c, x.other, y.other, p + ".other"); continue; }
        c.eqf(x.lod_dist, y.lod_dist, p + ".lod_dist"); c.eq(x.flags, y.flags, p + ".flags"); c.eq(x.special_attribute, y.special_attribute, p + ".special");
        c.eqv(x.bb_min, y.bb_min, p + ".bb_min"); c.eqv(x.bb_max, y.bb_max, p + ".bb_max"); c.eqv(x.bs_centre, y.bs_centre, p + ".bs_centre");
        c.eqf(x.bs_radius, y.bs_radius, p + ".bs_radius"); c.eqf(x.hd_texture_dist, y.hd_texture_dist, p + ".hd");
        c.eq(x.name, y.name, p + ".name"); c.eq(x.texture_dictionary, y.texture_dictionary, p + ".txd"); c.eq(x.clip_dictionary, y.clip_dictionary, p + ".clip");
        c.eq(x.drawable_dictionary, y.drawable_dictionary, p + ".ydd"); c.eq(x.physics_dictionary, y.physics_dictionary, p + ".phys");
        c.eq(x.asset_type, y.asset_type, p + ".asset_type"); c.eq(x.asset_name, y.asset_name, p + ".asset_name");
        compare_objects(c, x.extensions, y.extensions, p + ".ext");
        c.eq(x.time_flags, y.time_flags, p + ".time_flags"); c.eq(x.mlo_flags, y.mlo_flags, p + ".mlo_flags");
        compare_entities(c, x.entities, y.entities, p + ".entities");
        c.eq(x.rooms.size(), y.rooms.size(), p + ".rooms.count");
        for (size_t k = 0; k < x.rooms.size() && k < y.rooms.size(); k++) {
            auto& r = x.rooms[k]; auto& s = y.rooms[k];
            std::string rp = p + ".room[" + std::to_string(k) + "]";
            c.eq(r.name, s.name, rp + ".name"); c.eqv(r.bb_min, s.bb_min, rp + ".bb_min"); c.eqv(r.bb_max, s.bb_max, rp + ".bb_max"); c.eqf(r.blend, s.blend, rp + ".blend");
            c.eq(r.timecycle_name, s.timecycle_name, rp + ".tc"); c.eq(r.secondary_timecycle_name, s.secondary_timecycle_name, rp + ".tc2"); c.eq(r.flags, s.flags, rp + ".flags");
            c.eq(r.portal_count, s.portal_count, rp + ".portal_count"); c.eq(r.floor_id, s.floor_id, rp + ".floor"); c.eq(r.exterior_visibility_depth, s.exterior_visibility_depth, rp + ".depth");
            c.eq(r.attached_objects, s.attached_objects, rp + ".attached");
        }
        c.eq(x.portals.size(), y.portals.size(), p + ".portals.count");
        for (size_t k = 0; k < x.portals.size() && k < y.portals.size(); k++) {
            auto& r = x.portals[k]; auto& s = y.portals[k];
            std::string rp = p + ".portal[" + std::to_string(k) + "]";
            c.eq(r.room_from, s.room_from, rp + ".from"); c.eq(r.room_to, s.room_to, rp + ".to"); c.eq(r.flags, s.flags, rp + ".flags");
            c.eq(r.mirror_priority, s.mirror_priority, rp + ".mirror"); c.eq(r.opacity, s.opacity, rp + ".opacity"); c.eq(r.audio_occlusion, s.audio_occlusion, rp + ".audio");
            c.eq(r.corners.size(), s.corners.size(), rp + ".corners.count");
            for (size_t q = 0; q < r.corners.size() && q < s.corners.size(); q++) c.eqv(r.corners[q], s.corners[q], rp + ".corner");
            c.eq(r.corner_padding.size(), s.corner_padding.size(), rp + ".corner_padding.count");
            for (size_t q = 0; q < r.corner_padding.size() && q < s.corner_padding.size(); q++) c.eqf(r.corner_padding[q], s.corner_padding[q], rp + ".corner_padding");
            c.eq(r.attached_objects, s.attached_objects, rp + ".attached");
        }
        c.eq(x.entity_sets.size(), y.entity_sets.size(), p + ".sets.count");
        for (size_t k = 0; k < x.entity_sets.size() && k < y.entity_sets.size(); k++) {
            c.eq(x.entity_sets[k].name, y.entity_sets[k].name, p + ".set.name"); c.eq(x.entity_sets[k].locations, y.entity_sets[k].locations, p + ".set.locations");
            compare_entities(c, x.entity_sets[k].entities, y.entity_sets[k].entities, p + ".set.entities");
        }
        c.eq(x.time_cycle_modifiers.size(), y.time_cycle_modifiers.size(), p + ".tcm.count");
        for (size_t k = 0; k < x.time_cycle_modifiers.size() && k < y.time_cycle_modifiers.size(); k++) {
            auto& r = x.time_cycle_modifiers[k]; auto& s = y.time_cycle_modifiers[k];
            c.eq(r.name, s.name, p + ".tcm.name"); c.eqv(r.sphere, s.sphere, p + ".tcm.sphere"); c.eqf(r.percentage, s.percentage, p + ".tcm.pct");
            c.eqf(r.range, s.range, p + ".tcm.range"); c.eq(r.start_hour, s.start_hour, p + ".tcm.start"); c.eq(r.end_hour, s.end_hour, p + ".tcm.end");
        }
    }
}
static void compare_map(Cmp& c, const MapData& a, const MapData& b) {
    c.eq(a.name, b.name, "name"); c.eq(a.parent, b.parent, "parent"); c.eq(a.flags, b.flags, "flags"); c.eq(a.content_flags, b.content_flags, "content_flags");
    c.eqv(a.streaming_extents_min, b.streaming_extents_min, "stream_min"); c.eqv(a.streaming_extents_max, b.streaming_extents_max, "stream_max");
    c.eqv(a.entities_extents_min, b.entities_extents_min, "ent_min"); c.eqv(a.entities_extents_max, b.entities_extents_max, "ent_max");
    compare_entities(c, a.entities, b.entities, "entities");
    c.eq(a.box_occluders == b.box_occluders, true, "box_occluders");
    c.eq(a.physics_dictionaries, b.physics_dictionaries, "physics_dictionaries");
    c.eq(a.time_cycle_modifiers.size(), b.time_cycle_modifiers.size(), "tcm.count");
    for (size_t i = 0; i < a.time_cycle_modifiers.size() && i < b.time_cycle_modifiers.size(); i++) {
        auto& r = a.time_cycle_modifiers[i]; auto& s = b.time_cycle_modifiers[i];
        c.eq(r.name, s.name, "tcm.name"); c.eqv(r.min_extents, s.min_extents, "tcm.min"); c.eqv(r.max_extents, s.max_extents, "tcm.max");
        c.eqf(r.percentage, s.percentage, "tcm.pct"); c.eqf(r.range, s.range, "tcm.range"); c.eq(r.start_hour, s.start_hour, "tcm.start"); c.eq(r.end_hour, s.end_hour, "tcm.end");
    }
    c.eq(a.car_generators.size(), b.car_generators.size(), "cargens.count");
    for (size_t i = 0; i < a.car_generators.size() && i < b.car_generators.size(); i++) {
        auto& r = a.car_generators[i]; auto& s = b.car_generators[i];
        c.eqv(r.position, s.position, "cargen.pos"); c.eqf(r.orient_x, s.orient_x, "cargen.ox"); c.eqf(r.orient_y, s.orient_y, "cargen.oy");
        c.eqf(r.perpendicular_length, s.perpendicular_length, "cargen.len"); c.eq(r.car_model, s.car_model, "cargen.model"); c.eq(r.flags, s.flags, "cargen.flags");
        for (int k = 0; k < 4; k++) c.eq(r.body_color_remap[k], s.body_color_remap[k], "cargen.remap");
        c.eq(r.pop_group, s.pop_group, "cargen.pop"); c.eq(r.livery, s.livery, "cargen.livery");
    }
    c.eq(a.block.version, b.block.version, "block.version"); c.eq(a.block.flags, b.block.flags, "block.flags"); c.eq(a.block.name, b.block.name, "block.name");
    c.eq(a.block.exported_by, b.block.exported_by, "block.exported_by"); c.eq(a.block.owner, b.block.owner, "block.owner"); c.eq(a.block.time, b.block.time, "block.time");
    compare_objects(c, a.occlude_models, b.occlude_models, "occlude_models");
    compare_objects(c, a.container_lods, b.container_lods, "container_lods");
    compare_object(c, a.instanced_data, b.instanced_data, "instanced_data");
    compare_object(c, a.lod_lights, b.lod_lights, "lod_lights");
    compare_object(c, a.distant_lod_lights, b.distant_lod_lights, "distant_lod_lights");
}

// .rel audio data: read -> write is byte-identical, the typed dat151 interior items re-encode to their own bytes
static bool test_rel(const std::string& label, const Bytes& file) {
    Cmp c;
    size_t items = 0;
    try {
        Rel r = read_rel(file);
        Bytes w = write_rel(r); mix(w);
        if (w != file) {
            size_t i = 0; while (i < w.size() && i < file.size() && w[i] == file[i]) i++;
            c.fail("bytes differ at " + std::to_string(i) + " (in " + std::to_string(file.size()) + ", out " + std::to_string(w.size()) + ")");
        }
        std::string v = r.validate();
        if (!v.empty()) c.fail("validate: " + v);
        for (auto& it : r.items()) {
            items++;
            Bytes again;
            if (r.type == reltype::Dat151 || r.type == reltype::Dat149 || r.type == reltype::Dat150) {
                if (it.type_id == dat151::InteriorSettings) again = Dat151InteriorSettings::read(it).bytes();
                else if (it.type_id == dat151::InteriorRoom) again = Dat151InteriorRoom::read(it).bytes();
                else if (it.type_id == dat151::StaticEmitterList) again = Dat151StaticEmitterList::read(it).bytes();
                else if (it.type_id == dat151::DoorAudioSettingsLink) again = Dat151DoorAudioSettingsLink::read(it).bytes();
                else continue;
                if (again.size() != it.bytes.size() || std::memcmp(again.data(), it.bytes.data(), again.size()) != 0)
                    c.fail("typed item " + std::to_string(it.type_id) + " " + hash_name(it.hash) + " does not re-encode to its bytes");
                // the hash table lists exactly the reference fields the typed item names
                std::vector<uint32_t> want;
                if (it.type_id == dat151::InteriorSettings) want = Dat151InteriorSettings::read(it).refs();
                else if (it.type_id == dat151::InteriorRoom) want = Dat151InteriorRoom::read(it).refs();
                else if (it.type_id == dat151::StaticEmitterList) want = Dat151StaticEmitterList::read(it).refs();
                else want = Dat151DoorAudioSettingsLink::read(it).refs();
                std::vector<uint32_t> have;
                for (uint32_t p : r.hash_table) if (p >= it.offset + 8 && p < it.offset + 8 + it.length) have.push_back(p - it.offset - 8);
                if (have != want) c.fail("hash table entries of " + std::string(dat151_type_name(it.type_id)) + " " + hash_name(it.hash) + " (" + std::to_string(have.size()) + ") differ from the typed refs (" + std::to_string(want.size()) + ")");
            }
        }
        // the hash table must point at u32 fields (the game's hash references): every position readable
        for (uint32_t p : r.hash_table) { uint32_t h; if (!r.hash_at(p, h)) c.fail("hash table position out of range"); }
    } catch (const std::exception& e) {
        std::printf("FAIL  %-60s  %s\n", label.c_str(), e.what());
        return false;
    }
    if (c.diffs) { std::printf("DIFF  %-60s  %d differences, first: %s\n", label.c_str(), c.diffs, c.first_diff.c_str()); return false; }
    std::printf("ok    %-60s  %zu bytes, %zu items\n", label.c_str(), file.size(), items);
    return true;
}

static bool test_bytes(const std::string& label, const std::string& ext, const Bytes& file) {
    g_digest = 0;
    if (ext == ".rel") return test_rel(label, file);
    if ((ext == ".meta" || ext == ".xml") && !is_rbf(file) && !is_pso(file)) {
        // XML data files (vehicles.meta, carcols, handling, peds ...): parse -> write -> parse must give the same document
        try {
            const XmlElement a = parse_xml(std::string_view((const char*)file.data(), file.size()));
            const std::string w = write_xml(a);
            mix(Bytes(w.begin(), w.end()));
            if (!(parse_xml(w) == a)) {
                std::printf("DIFF  %-60s  the written XML reads back as a different document\n", label.c_str());
                return false;
            }
        } catch (const std::exception& e) {
            std::printf("FAIL  %-60s  %s\n", label.c_str(), e.what());
            return false;
        }
        std::printf("ok    %-60s  %zu bytes, XML\n", label.c_str(), file.size());
        return true;
    }
    if (is_rbf(file)) {
        // RBF (creature metadata, cutscene / Bink metadata): read -> write must give the identical bytes
        try {
            Bytes w = write_rbf(read_rbf(file)); mix(w);
            if (w != file) {
                size_t i = 0; while (i < w.size() && i < file.size() && w[i] == file[i]) i++;
                std::printf("DIFF  %-60s  RBF bytes differ at %zu (in %zu, out %zu)\n", label.c_str(), i, file.size(), w.size());
                return false;
            }
        } catch (const std::exception& e) {
            std::printf("FAIL  %-60s  %s\n", label.c_str(), e.what());
            return false;
        }
        std::printf("ok    %-60s  %zu bytes, RBF\n", label.c_str(), file.size());
        return true;
    }
    bool pso = is_pso(file) && (ext == ".ymt" || ext == ".ypdb" || ext == ".ymf");
    if (!pso && !rsc::Resource::is_resource(file)) return true;
    Cmp c;
    size_t out_size = 0;
    try {
        if (pso) {
            // PSO: read -> write must give the identical bytes; the generic dump must walk without throwing
            Pso a = read_pso(file); Bytes w = write_pso(a); mix(w); out_size = w.size();
            if (w != file) {
                size_t i = 0; while (i < w.size() && i < file.size() && w[i] == file[i]) i++;
                c.fail("bytes differ at " + std::to_string(i) + " (in " + std::to_string(file.size()) + ", out " + std::to_string(w.size()) + ")");
            }
            if (a.has_checks) {
                Pso t = a; t.update_checksum();
                if (t.checks.file_size != a.checks.file_size || t.checks.checksum != a.checks.checksum) c.fail("CHKS differs from the recomputed size/checksum");
            }
            for (auto& sec : a.sections) if (!sec.parsed && (sec.ident == kPsoPMAP || sec.ident == kPsoPSCH)) c.fail("section kept raw: not canonical");
            if (a.has_map && a.has_schema) (void)pso_dump(a, 0);
            // occlusion: the typed model must survive read -> write -> read (the written file is CodeWalker's layout, not the original bytes)
            if (a.has_map && a.has_schema && PsoView(a).root().type == joaat("naOcclusionInteriorMetadata")) {
                OcclusionData o1 = read_occlusion_ymt(file); Bytes ow = write_occlusion_ymt(o1); mix(ow); OcclusionData o2 = read_occlusion_ymt(ow);
                bool ok = o1.portals.size() == o2.portals.size() && o1.nodes.size() == o2.nodes.size();
                for (size_t i = 0; ok && i < o1.portals.size(); i++) {
                    auto &x = o1.portals[i], &y = o2.portals[i];
                    ok = x.interior_proxy_hash == y.interior_proxy_hash && x.portal_idx == y.portal_idx && x.room_idx == y.room_idx &&
                         x.dest_interior_hash == y.dest_interior_hash && x.dest_room_idx == y.dest_room_idx && x.entities.size() == y.entities.size();
                    for (size_t k = 0; ok && k < x.entities.size(); k++) {
                        auto &e = x.entities[k], &f = y.entities[k];
                        ok = e.link_type == f.link_type && e.max_occlusion == f.max_occlusion && e.model_hash == f.model_hash && e.is_door == f.is_door && e.is_glass == f.is_glass;
                    }
                }
                for (size_t i = 0; ok && i < o1.nodes.size(); i++) {
                    auto &x = o1.nodes[i], &y = o2.nodes[i];
                    ok = x.key == y.key && x.children.size() == y.children.size();
                    for (size_t k = 0; ok && k < x.children.size(); k++) ok = x.children[k].path_node_key == y.children[k].path_node_key && x.children[k].portal_info_idx == y.children[k].portal_info_idx;
                }
                if (!ok) c.fail("occlusion: read_occlusion_ymt(write_occlusion_ymt(read_occlusion_ymt)) differs");
            }
            // manifests: the typed model must survive read -> write -> read (the written file is CodeWalker's layout, not the original bytes)
            if (a.has_map && a.has_schema && PsoView(a).root().type == joaat("CPackFileMetaData")) {
                Manifest m1 = read_ymf(file); Bytes mw = write_ymf(m1); mix(mw); Manifest m2 = read_ymf(mw);
                auto same = [](const std::vector<ManifestDependencies>& x, const std::vector<ManifestDependencies>& y) {
                    if (x.size() != y.size()) return false;
                    for (size_t i = 0; i < x.size(); i++) if (x[i].name != y[i].name || x[i].flags != y[i].flags || x[i].deps != y[i].deps) return false;
                    return true;
                };
                bool ok = same(m1.imap_dependencies_2, m2.imap_dependencies_2) && same(m1.ityp_dependencies_2, m2.ityp_dependencies_2);
                ok = ok && m1.interiors.size() == m2.interiors.size() && m1.map_data_groups.size() == m2.map_data_groups.size()
                        && m1.hd_txd_bindings.size() == m2.hd_txd_bindings.size() && m1.imap_dependencies.size() == m2.imap_dependencies.size();
                for (size_t i = 0; ok && i < m1.interiors.size(); i++) ok = m1.interiors[i].name == m2.interiors[i].name && m1.interiors[i].bounds == m2.interiors[i].bounds;
                for (size_t i = 0; ok && i < m1.map_data_groups.size(); i++) {
                    auto &x = m1.map_data_groups[i], &y = m2.map_data_groups[i];
                    ok = x.name == y.name && x.bounds == y.bounds && x.flags == y.flags && x.weather_types == y.weather_types && x.hours_on_off == y.hours_on_off;
                }
                for (size_t i = 0; ok && i < m1.hd_txd_bindings.size(); i++) {
                    auto &x = m1.hd_txd_bindings[i], &y = m2.hd_txd_bindings[i];
                    ok = x.asset_type == y.asset_type && x.target_asset == y.target_asset && x.hd_txd == y.hd_txd;
                }
                for (size_t i = 0; ok && i < m1.imap_dependencies.size(); i++) {
                    auto &x = m1.imap_dependencies[i], &y = m2.imap_dependencies[i];
                    ok = x.imap_name == y.imap_name && x.ityp_name == y.ityp_name && x.pack_file_name == y.pack_file_name;
                }
                if (!ok) c.fail("typed manifest: read_ymf(write_ymf(read_ymf)) differs");
                if (mw != file) std::printf("note  %s: write_ymf bytes differ from the original (%zu vs %zu)\n", label.c_str(), mw.size(), file.size());
            }
        } else if (ext == ".ytd") {
            auto a = read_ytd(file); Bytes w = write_ytd(a); mix(w); out_size = w.size(); auto b = read_ytd(w); compare_txd(c, a, b, "txd");
        } else if (ext == ".ydr") {
            auto a = read_ydr(file); Bytes w = write_ydr(a); mix(w); out_size = w.size(); auto b = read_ydr(w); compare_drawable(c, a, b, "ydr");
        } else if (ext == ".ybn") {
            auto a = read_ybn(file); Bytes w = write_ybn(*a); mix(w); out_size = w.size(); auto b = read_ybn(w); compare_bound(c, *a, *b, "ybn");
        } else if (ext == ".ydd") {
            auto a = read_ydd(file); Bytes w = write_ydd(a); mix(w); out_size = w.size(); auto b = read_ydd(w);
            c.eq(a.drawables.size(), b.drawables.size(), "ydd.count");
            // the writer sorts by hash (stable), so sort the input the same way and match by position
            std::vector<size_t> order(a.drawables.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return a.hashes[x] < a.hashes[y]; });
            for (size_t i = 0; i < order.size() && i < b.drawables.size(); i++) {
                c.eq(a.hashes[order[i]], b.hashes[i], "ydd.hash");
                compare_drawable(c, a.drawables[order[i]], b.drawables[i], "ydd[" + hash_name(a.hashes[order[i]]) + "]");
            }
        } else if (ext == ".ymt") {
            // RSC7 .ymt (ped variation infos, clothing, vehicle metadata ...) are plain Meta files: generic layer only
            Meta a = read_meta(file); Bytes w = write_meta(a); mix(w); out_size = w.size(); Meta b = read_meta(w);
            compare_meta(c, a, b, "meta");
        } else if (ext == ".ytyp" || ext == ".ymap") {
            Meta a = read_meta(file); Bytes w = write_meta(a); mix(w); out_size = w.size(); Meta b = read_meta(w);
            compare_meta(c, a, b, "meta");
            // typed model: read -> write -> read, compare; the written file is also read as generic meta
            if (ext == ".ytyp") {
                MapTypes ta = read_ytyp(file); Bytes tw = write_ytyp(ta); mix(tw); MapTypes tb = read_ytyp(tw); compare_types(c, ta, tb);
                Meta mw = read_meta(tw); if (mw.root_block != 1) c.fail("typed write: root block is not 1");
                compare_struct_items(c, a, mw);
                out_size = tw.size();
            } else {
                MapData ta = read_ymap(file); Bytes tw = write_ymap(ta); mix(tw); MapData tb = read_ymap(tw); compare_map(c, ta, tb);
                Meta mw = read_meta(tw); if (mw.root_block != 1) c.fail("typed write: root block is not 1");
                compare_struct_items(c, a, mw);
                out_size = tw.size();
            }
        } else return true;
    } catch (const std::exception& e) {
        std::printf("FAIL  %-60s  %s\n", label.c_str(), e.what());
        return false;
    }
    if (c.diffs) {
        std::printf("DIFF  %-60s  %d differences, first: %s\n", label.c_str(), c.diffs, c.first_diff.c_str());
        return false;
    }
    std::printf("ok    %-60s  %zu -> %zu bytes\n", label.c_str(), file.size(), out_size);
    return true;
}

static bool test_file(const fs::path& path) {
    Bytes file;
    try { file = rsc::read_file(path.string()); } catch (...) { return true; }
    return test_bytes(path.filename().string(), lower(path.extension().string()), file);
}

static bool tested_ext(const std::string& ext) {
    return ext == ".rel" || ext == ".ytd" || ext == ".ydr" || ext == ".ydd" || ext == ".ybn" || ext == ".ytyp" || ext == ".ymap" || ext == ".ymt" || ext == ".ypdb" || ext == ".ymf" ||
           ext == ".meta" || ext == ".xml";
}

static std::map<std::string, uint64_t> g_recorded, g_written;
static int g_changed = 0;

static void note_digest(const std::string& label) {
    if (!g_digest) return;
    g_written[label] = g_digest;
    auto it = g_recorded.find(label);
    if (it != g_recorded.end() && it->second != g_digest) { std::printf("CHANGED  %s\n", label.c_str()); g_changed++; }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: rk_roundtrip [--only=.ytyp,.ymap,.rel] [--record=f | --check=f] <file or directory>...\n"
                    "       rk_roundtrip --game[=install dir] [--every=N] [--max=N] [--only=...] [--record=f | --check=f]\n");
        return 2;
    }
    int failed = 0, total = 0;
    std::string only, game, record, check; // only: comma separated extensions, e.g. ".ytyp,.ymap"
    bool use_game = false;
    size_t every = 1, max_per_ext = SIZE_MAX;
    std::vector<fs::path> paths;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a.rfind("--only=", 0) == 0) only = "," + lower(a.substr(7)) + ",";
        else if (a == "--game") use_game = true;
        else if (a.rfind("--game=", 0) == 0) { use_game = true; game = a.substr(7); }
        else if (a.rfind("--every=", 0) == 0) every = std::max<size_t>(1, std::stoul(a.substr(8)));
        else if (a.rfind("--max=", 0) == 0) max_per_ext = std::stoul(a.substr(6));
        else if (a.rfind("--record=", 0) == 0) record = a.substr(9);
        else if (a.rfind("--check=", 0) == 0) check = a.substr(8);
        else paths.push_back(a);
    }
    if (!check.empty()) {
        std::ifstream in(check);
        std::string label; uint64_t d;
        while (in >> std::hex >> d && std::getline(in >> std::ws, label)) g_recorded[label] = d;
        std::printf("checking against %zu recorded digests\n", g_recorded.size());
    }
    auto wanted = [&](const std::string& ext) { return tested_ext(ext) && (only.empty() || only.find("," + ext + ",") != std::string::npos); };
    if (use_game) {
        GameIndex gi = GameIndex::open(game.empty() ? GameIndex::default_game_dir() : game);
        std::map<std::string, size_t> seen, taken;
        for (const IndexedFile& f : gi.files()) {
            const std::string& ext = gi.extension(f);
            if (!wanted(ext) || seen[ext]++ % every != 0 || taken[ext] >= max_per_ext) continue;
            taken[ext]++;
            total++;
            const std::string label = gi.path(f);
            Bytes bytes;
            try { bytes = gi.extract(f); } catch (const std::exception& e) { std::printf("FAIL  %-60s  extract: %s\n", label.c_str(), e.what()); failed++; continue; }
            if (!test_bytes(label, ext, bytes)) failed++;
            note_digest(label);
        }
    }
    for (const fs::path& p : paths) {
        if (fs::is_directory(p)) {
            for (auto& e : fs::recursive_directory_iterator(p)) {
                if (!e.is_regular_file() || !wanted(lower(e.path().extension().string()))) continue;
                total++;
                if (!test_file(e.path())) failed++;
                note_digest(e.path().filename().string());
            }
        } else { total++; if (!test_file(p)) failed++; note_digest(p.filename().string()); }
    }
    if (!record.empty()) {
        std::ofstream out(record);
        for (auto& [label, d] : g_written) { char b[24]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)d); out << b << ' ' << label << '\n'; }
    }
    std::printf("\n%d files, %d failed", total, failed);
    if (!check.empty()) std::printf(", %d written differently than recorded", g_changed);
    std::printf("\n");
    return failed || g_changed ? 1 : 0;
}
