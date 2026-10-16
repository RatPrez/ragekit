// rk_roundtrip: reads every resource under the given paths (or a sample of a game install, --game), writes it back, reads
// the result, and compares the two models field by field. Prints one line per file. --record / --check keep a digest of
// everything written per file, so a rewrite can be held to the bytes the previous code wrote.
#include <algorithm>
#include <cmath>
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
#include "cloth_io.h"

namespace fs = std::filesystem;
using namespace rk;

static uint64_t g_digest = 0;
static bool g_untested = false;   // test_bytes looked at the file but had nothing to test (a .yft without cloth)
static void mix(const Bytes& b) {
    uint64_t h = g_digest ? g_digest : 1469598103934665603ull;
    for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
    g_digest = h ^ b.size();
}

static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

struct Cmp {
    std::string first_diff;
    int diffs = 0;
    bool cross = false;   // comparing a file with its write in the other edition: only what both editions store
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
    c.eq(a.usage(), b.usage(), p + ".usage");
    c.eq(a.data, b.data, p + ".data");
    if (c.cross) return;
    c.eq(a.usage_data, b.usage_data, p + ".usage_data");
    c.eq(a.extra_flags, b.extra_flags, p + ".extra_flags");
    c.eq(a.unk32, b.unk32, p + ".unk32");
    c.eq(a.gen9.has_value(), b.gen9.has_value(), p + ".gen9");
    if (a.gen9 && b.gen9) {
        auto& x = *a.gen9; auto& y = *b.gen9;
        c.eq(x.format, y.format, p + ".gen9.format"); c.eq(x.dimension, y.dimension, p + ".gen9.dimension"); c.eq(x.tile_mode, y.tile_mode, p + ".gen9.tile_mode");
        c.eq(x.unk23, y.unk23, p + ".gen9.unk23"); c.eq(x.flags, y.flags, p + ".gen9.flags"); c.eq(x.srv_vft, y.srv_vft, p + ".gen9.srv_vft");
    }
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
            c.eq(x.name_hash, y.name_hash, sp + ".name");
            c.eq(x.render_bucket, y.render_bucket, sp + ".bucket");
            if (!c.cross) { c.eq(x.sps_hash, y.sps_hash, sp + ".sps"); c.eq(x.unk12, y.unk12, sp + ".unk12"); }   // Enhanced stores no preset
            if (!c.cross) {
                c.eq(x.gen9.has_value(), y.gen9.has_value(), sp + ".gen9");
                if (x.gen9 && y.gen9) {
                    auto& u = *x.gen9; auto& v = *y.gen9;
                    c.eq(u.params.size(), v.params.size(), sp + ".gen9.params.count");
                    for (size_t k = 0; k < u.params.size() && k < v.params.size(); k++) { c.eq(u.params[k].name_hash, v.params[k].name_hash, sp + ".gen9.param.name"); c.eq(u.params[k].data, v.params[k].data, sp + ".gen9.param.data"); }
                    c.eq(u.buffer_sizes, v.buffer_sizes, sp + ".gen9.buffer_sizes"); c.eq(u.buffers, v.buffers, sp + ".gen9.buffers");
                    c.eq(u.textures, v.textures, sp + ".gen9.textures"); c.eq(u.samplers, v.samplers, sp + ".gen9.samplers");
                    c.eq(u.unknowns, v.unknowns, sp + ".gen9.unknowns"); c.eq(u.copies, v.copies, sp + ".gen9.copies");
                }
            }
            c.eq(x.params.size(), y.params.size(), sp + ".params.count");
            for (size_t k = 0; k < x.params.size() && k < y.params.size(); k++) {
                auto& u = x.params[k]; auto& v = y.params[k];
                std::string pp = sp + ".param[" + hash_name(u.name_hash) + "]";
                c.eq(u.name_hash, v.name_hash, pp + ".name"); c.eq(u.type, v.type, pp + ".type");
                if (c.cross) c.eq(lower(u.texture), lower(v.texture), pp + ".texture");   // the other edition may resolve it to an embedded texture of another case
                else c.eq(u.texture, v.texture, pp + ".texture");
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
                c.eq(u.layout.flags, v.layout.flags, gp + ".layout.flags");
                c.eq(u.vertex_count, v.vertex_count, gp + ".vertex_count");
                if (c.cross && u.layout.types != v.layout.types && u.layout.flags == v.layout.flags && u.vertex_count == v.vertex_count) {
                    // converted between layouts (cloth: Legacy's half floats, Enhanced's floats): the same values, to half precision
                    for (int sem = 0; sem < 16; sem++) {
                        if (!u.layout.has((VertexSemantic)sem)) continue;
                        for (uint32_t k2 = 0; k2 < u.vertex_count; k2++) {
                            const Vec4 x = u.get(k2, (VertexSemantic)sem), y = v.get(k2, (VertexSemantic)sem);
                            const float d = std::max({std::fabs(x.x - y.x), std::fabs(x.y - y.y), std::fabs(x.z - y.z), std::fabs(x.w - y.w)});
                            if (d > 0.01f * std::max(1.f, std::fabs(x.x) + std::fabs(x.y) + std::fabs(x.z))) { c.fail(gp + ".vertices (converted)"); break; }
                        }
                    }
                } else {
                    c.eq(u.layout.types, v.layout.types, gp + ".layout.types");
                    c.eq(u.vertices, v.vertices, gp + ".vertices");
                }
                c.eq(u.indices, v.indices, gp + ".indices"); c.eq(u.shader_index, v.shader_index, gp + ".shader");
                c.eq(u.bone_ids, v.bone_ids, gp + ".bone_ids"); c.eq(u.vb_flags, v.vb_flags, gp + ".vb_flags"); c.eq(u.vb_bind_gen9, v.vb_bind_gen9, gp + ".vb_bind_gen9");
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
        c.eq(x.unk45, y.unk45, lp + ".unk45"); c.eq(x.unk46, y.unk46, lp + ".unk46"); c.eq(x.extra_flags, y.extra_flags, lp + ".extra_flags");
    }
}

static void compare_bound(Cmp& c, const Bound& a, const Bound& b, const std::string& p) {
    c.eq(a.type, b.type, p + ".type");
    c.eqv(a.box_min, b.box_min, p + ".box_min"); c.eqv(a.box_max, b.box_max, p + ".box_max"); c.eqv(a.box_center, b.box_center, p + ".box_center");
    c.eqv(a.sphere_center, b.sphere_center, p + ".sphere_center"); c.eqf(a.sphere_radius, b.sphere_radius, p + ".sphere_radius"); c.eqf(a.margin, b.margin, p + ".margin");
    c.eq(a.material_index, b.material_index, p + ".material"); c.eq(a.room_ped_density, b.room_ped_density, p + ".room"); c.eq(a.unk_flags, b.unk_flags, p + ".unk_flags");
    c.eq(a.poly_flags, b.poly_flags, p + ".poly_flags"); c.eqf(a.volume, b.volume, p + ".volume"); c.eqv(a.inertia, b.inertia, p + ".inertia");
    c.eq(a.unk3C, b.unk3C, p + ".unk3C"); c.eq(a.procedural_id, b.procedural_id, p + ".procedural_id"); c.eq(a.material_colour_index, b.material_colour_index, p + ".material_colour");
    c.eq(a.unk5E, b.unk5E, p + ".unk5E"); c.eqf(a.unk70, b.unk70, p + ".unk70");
    c.eq(a.has_child_flags, b.has_child_flags, p + ".has_child_flags"); c.eq(a.has_child_transforms2, b.has_child_transforms2, p + ".has_child_transforms2");
    c.eq(a.child_transforms2.size(), b.child_transforms2.size(), p + ".child_transforms2.count");
    for (size_t i = 0; i < a.child_transforms2.size() && i < b.child_transforms2.size(); i++)
        if (std::memcmp(&a.child_transforms2[i], &b.child_transforms2[i], sizeof(BoundTransform)) != 0) c.fail(p + ".child_transform2");
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

// Cloth: every array element bit for bit (floats included), every value field, every nested structure and bound.
template <class T> static void eq_bits(Cmp& c, const std::vector<T>& a, const std::vector<T>& b, const std::string& what) {
    if (a.size() != b.size()) { c.fail(what + ".count"); return; }
    for (size_t i = 0; i < a.size(); i++) if (std::memcmp(&a[i], &b[i], sizeof(T)) != 0) { c.fail(what + "[" + std::to_string(i) + "]"); return; }
}
static void compare_bound_ptr(Cmp& c, const std::shared_ptr<Bound>& a, const std::shared_ptr<Bound>& b, const std::string& p) {
    c.eq((bool)a, (bool)b, p + ".present");
    if (a && b) compare_bound(c, *a, *b, p);
}
static void compare_verlet(Cmp& c, const VerletCloth& a, const VerletCloth& b, const std::string& p) {
    eq_bits(c, a.vertices, b.vertices, p + ".vertices"); eq_bits(c, a.vertices2, b.vertices2, p + ".vertices2");
    eq_bits(c, a.edges, b.edges, p + ".edges"); eq_bits(c, a.custom_edges, b.custom_edges, p + ".custom_edges");
    compare_bound_ptr(c, a.bound, b.bound, p + ".bound");
    c.eqv(a.bbox_min, b.bbox_min, p + ".bbox_min"); c.eqv(a.bbox_max, b.bbox_max, p + ".bbox_max"); c.eq(a.unk3C, b.unk3C, p + ".unk3C"); c.eq(a.unk4C, b.unk4C, p + ".unk4C");
    c.eqf(a.unkA8, b.unkA8, p + ".unkA8"); c.eqf(a.unkAC, b.unkAC, p + ".unkAC"); c.eq(a.unkE8, b.unkE8, p + ".unkE8");
    c.eq(a.unkFA, b.unkFA, p + ".unkFA"); c.eq(a.unk148, b.unk148, p + ".unk148"); c.eqf(a.unk158, b.unk158, p + ".unk158");
}
static void compare_cloth_controller(Cmp& c, const ClothController& a, const ClothController& b, const std::string& p) {
    c.eq(a.name, b.name, p + ".name"); c.eq(a.type, b.type, p + ".type"); c.eqf(a.unk78, b.unk78, p + ".unk78");
    c.eq(a.bridge.has_value(), b.bridge.has_value(), p + ".has_bridge");
    if (a.bridge && b.bridge) {
        auto& x = *a.bridge; auto& y = *b.bridge;
        for (int l = 0; l < kClothLods; l++) {
            std::string lp = p + ".bridge.lod" + std::to_string(l);
            c.eq(x.vertex_counts[l], y.vertex_counts[l], lp + ".vertex_count");
            eq_bits(c, x.unk20[l], y.unk20[l], lp + ".unk20"); eq_bits(c, x.unk60[l], y.unk60[l], lp + ".unk60");
            eq_bits(c, x.unkA0[l], y.unkA0[l], lp + ".unkA0"); eq_bits(c, x.vertex_maps[l], y.vertex_maps[l], lp + ".vertex_map");
        }
        eq_bits(c, x.vertex_bits, y.vertex_bits, p + ".bridge.vertex_bits"); c.eq(x.bit_count, y.bit_count, p + ".bridge.bit_count");
    }
    c.eq(a.morph.has_value(), b.morph.has_value(), p + ".has_morph");
    if (a.morph && b.morph)
        for (int l = 0; l < kClothLods; l++) {
            auto& x = a.morph->maps[l]; auto& y = b.morph->maps[l];
            std::string mp = p + ".morph" + std::to_string(l);
            c.eq(x.has_value(), y.has_value(), mp + ".present");
            if (!x || !y) continue;
            eq_bits(c, x->unk50, y->unk50, mp + ".unk50"); eq_bits(c, x->unkA0, y->unkA0, mp + ".unkA0");
            for (int i = 0; i < 4; i++) { eq_bits(c, x->unk60[i], y->unk60[i], mp + ".unk60"); eq_bits(c, x->unkB0[i], y->unkB0[i], mp + ".unkB0"); }
            eq_bits(c, x->unk150, y->unk150, mp + ".unk150"); eq_bits(c, x->unk160, y->unk160, mp + ".unk160"); c.eq(x->unk180, y->unk180, mp + ".unk180");
        }
    for (int l = 0; l < kClothLods; l++) {
        c.eq(a.verlet[l].has_value(), b.verlet[l].has_value(), p + ".verlet" + std::to_string(l) + ".present");
        if (a.verlet[l] && b.verlet[l]) compare_verlet(c, *a.verlet[l], *b.verlet[l], p + ".verlet" + std::to_string(l));
    }
}
static void compare_character_cloth(Cmp& c, const CharacterCloth& a, const CharacterCloth& b, const std::string& p) {
    c.eq(a.controller.has_value(), b.controller.has_value(), p + ".has_controller");
    if (a.controller && b.controller) {
        auto& x = *a.controller; auto& y = *b.controller;
        compare_cloth_controller(c, x, y, p + ".controller");
        eq_bits(c, x.indices, y.indices, p + ".indices"); eq_bits(c, x.vertices, y.vertices, p + ".vertices");
        eq_bits(c, x.bone_weights, y.bone_weights, p + ".bone_weights"); eq_bits(c, x.bone_ids, y.bone_ids, p + ".controller.bone_ids");
        eq_bits(c, x.unkB0, y.unkB0, p + ".unkB0"); c.eqf(x.unkA0, y.unkA0, p + ".unkA0"); c.eqf(x.unkDC, y.unkDC, p + ".unkDC");
    }
    compare_bound_ptr(c, a.bound, b.bound, p + ".bound");
    eq_bits(c, a.bone_ids, b.bone_ids, p + ".bone_ids");
    for (int k = 0; k < 16; k++) c.eqf(a.transform.m[k], b.transform.m[k], p + ".transform");
    eq_bits(c, a.unk10, b.unk10, p + ".unk10"); eq_bits(c, a.unk90, b.unk90, p + ".unk90");
}
static void compare_environment_cloth(Cmp& c, const EnvironmentCloth& a, const EnvironmentCloth& b, const std::string& p) {
    c.eq(a.controller.has_value(), b.controller.has_value(), p + ".has_controller");
    if (a.controller && b.controller) compare_cloth_controller(c, *a.controller, *b.controller, p + ".controller");
    c.eq(a.tuning.has_value(), b.tuning.has_value(), p + ".has_tuning");
    if (a.tuning && b.tuning) {
        auto& x = *a.tuning; auto& y = *b.tuning;
        c.eqf(x.unk10, y.unk10, p + ".tuning.unk10"); c.eqf(x.unk14, y.unk14, p + ".tuning.unk14"); c.eqf(x.unk20, y.unk20, p + ".tuning.unk20");
        c.eqf(x.unk24, y.unk24, p + ".tuning.unk24"); c.eqf(x.unk28, y.unk28, p + ".tuning.unk28"); c.eqf(x.unk2C, y.unk2C, p + ".tuning.unk2C");
        c.eq(x.flags, y.flags, p + ".tuning.flags"); c.eqf(x.unk34, y.unk34, p + ".tuning.unk34"); c.eqf(x.unk38, y.unk38, p + ".tuning.unk38"); c.eq(x.unk3C, y.unk3C, p + ".tuning.unk3C");
    }
    eq_bits(c, a.unk60, b.unk60, p + ".unk60"); c.eq(a.unk78, b.unk78, p + ".unk78");
}

// Fragments: every field, every raw byte, every drawable, bound and cloth.
template <class T> static void eq_mem(Cmp& c, const T& a, const T& b, const std::string& p) {
    if (std::memcmp(&a, &b, sizeof(T)) != 0) c.fail(p);
}
static void compare_frag_drawable(Cmp& c, const FragDrawable& a, const FragDrawable& b, const std::string& p) {
    compare_drawable(c, a, b, p);
    eq_mem(c, a.frag_matrix, b.frag_matrix, p + ".frag_matrix");
    c.eq(a.frag_matrices_inds, b.frag_matrices_inds, p + ".frag_matrices_inds");
    c.eq(a.frag_matrices.size(), b.frag_matrices.size(), p + ".frag_matrices.count");
    for (size_t i = 0; i < a.frag_matrices.size() && i < b.frag_matrices.size(); i++) eq_mem(c, a.frag_matrices[i], b.frag_matrices[i], p + ".frag_matrices");
    c.eq(a.frag_matrices_count, b.frag_matrices_count, p + ".frag_matrices_count");
    compare_bound_ptr(c, a.bound, b.bound, p + ".bound");
}
static void compare_opt_frag_drawable(Cmp& c, const std::optional<FragDrawable>& a, const std::optional<FragDrawable>& b, const std::string& p) {
    c.eq(a.has_value(), b.has_value(), p + ".present");
    if (a && b) compare_frag_drawable(c, *a, *b, p);
}
static void compare_lod(Cmp& c, const FragPhysicsLod& a, const FragPhysicsLod& b, const std::string& p) {
    eq_mem(c, a.raw, b.raw, p + ".raw");
    c.eq(a.groups.size(), b.groups.size(), p + ".groups.count");
    for (size_t i = 0; i < a.groups.size() && i < b.groups.size(); i++) eq_mem(c, a.groups[i].raw, b.groups[i].raw, p + ".group");
    c.eq(a.children.size(), b.children.size(), p + ".children.count");
    for (size_t i = 0; i < a.children.size() && i < b.children.size(); i++) {
        auto& x = a.children[i]; auto& y = b.children[i];
        const std::string cp = p + ".child[" + std::to_string(i) + "]";
        eq_mem(c, x.raw, y.raw, cp + ".raw");
        c.eqf(x.pristine_mass, y.pristine_mass, cp + ".pristine_mass"); c.eqf(x.damaged_mass, y.damaged_mass, cp + ".damaged_mass");
        c.eq(x.group_index, y.group_index, cp + ".group_index"); c.eq(x.bone_tag, y.bone_tag, cp + ".bone_tag");
        compare_opt_frag_drawable(c, x.drawable, y.drawable, cp + ".drawable");
        compare_opt_frag_drawable(c, x.drawable_damaged, y.drawable_damaged, cp + ".damaged");
        c.eq(x.event_set.has_value(), y.event_set.has_value(), cp + ".has_event_set");
        if (x.event_set && y.event_set) eq_mem(c, *x.event_set, *y.event_set, cp + ".event_set");
        eq_mem(c, x.unk_float, y.unk_float, cp + ".unk_float"); eq_mem(c, x.unk_vec, y.unk_vec, cp + ".unk_vec"); eq_mem(c, x.inertia_tensor, y.inertia_tensor, cp + ".inertia");
    }
    for (int k = 0; k < 2; k++) {
        auto& x = k ? a.archetype_damaged : a.archetype; auto& y = k ? b.archetype_damaged : b.archetype;
        const std::string ap = p + (k ? ".archetype_damaged" : ".archetype");
        c.eq(x.has_value(), y.has_value(), ap + ".present");
        if (x && y) { eq_mem(c, x->raw, y->raw, ap + ".raw"); c.eq(x->name, y->name, ap + ".name"); compare_bound_ptr(c, x->bound, y->bound, ap + ".bound"); }
    }
    compare_bound_ptr(c, a.bound, b.bound, p + ".bound");
    c.eq(a.archetype && a.bound && a.archetype->bound == a.bound, b.archetype && b.bound && b.archetype->bound == b.bound, p + ".bound shared with the archetype's");
    c.eq(a.transforms.size(), b.transforms.size(), p + ".transforms.count");
    for (size_t i = 0; i < a.transforms.size() && i < b.transforms.size(); i++) eq_mem(c, a.transforms[i], b.transforms[i], p + ".transform");
    c.eq(a.articulated_body.has_value(), b.articulated_body.has_value(), p + ".has_articulated_body");
    if (a.articulated_body && b.articulated_body) {
        eq_mem(c, a.articulated_body->raw, b.articulated_body->raw, p + ".articulated.raw");
        c.eq(a.articulated_body->joints, b.articulated_body->joints, p + ".articulated.joints");
        c.eq(a.articulated_body->vectors.size(), b.articulated_body->vectors.size(), p + ".articulated.vectors.count");
        if (a.articulated_body->vectors.size() == b.articulated_body->vectors.size() && !a.articulated_body->vectors.empty() &&
            std::memcmp(a.articulated_body->vectors.data(), b.articulated_body->vectors.data(), a.articulated_body->vectors.size() * sizeof(Vec4)) != 0)
            c.fail(p + ".articulated.vectors");
    }
    c.eq(a.unknown_data1, b.unknown_data1, p + ".unknown_data1"); c.eq(a.unknown_data2, b.unknown_data2, p + ".unknown_data2");
}
static void compare_fragment(Cmp& c, const Fragment& a, const Fragment& b) {
    eq_mem(c, a.raw, b.raw, "yft.raw");
    c.eq(a.name, b.name, "yft.name");
    eq_mem(c, a.sphere_center, b.sphere_center, "yft.sphere_center"); eq_mem(c, a.sphere_radius, b.sphere_radius, "yft.sphere_radius");
    compare_opt_frag_drawable(c, a.drawable, b.drawable, "yft.drawable");
    c.eq(a.drawable_array.size(), b.drawable_array.size(), "yft.drawable_array.count");
    for (size_t i = 0; i < a.drawable_array.size() && i < b.drawable_array.size(); i++) compare_frag_drawable(c, a.drawable_array[i], b.drawable_array[i], "yft.drawable_array");
    c.eq(a.drawable_array_names, b.drawable_array_names, "yft.drawable_array_names");
    c.eq(a.bone_transforms, b.bone_transforms, "yft.bone_transforms"); c.eq(a.bone_transforms_unk, b.bone_transforms_unk, "yft.bone_transforms_unk");
    c.eq(a.glass_windows, b.glass_windows, "yft.glass_windows");
    if (!c.cross) c.eq(a.glass_declarations_gen9, b.glass_declarations_gen9, "yft.glass_declarations_gen9");
    for (int i = 0; i < 3; i++) {
        c.eq(a.physics[(size_t)i].has_value(), b.physics[(size_t)i].has_value(), "yft.physics.present");
        if (a.physics[(size_t)i] && b.physics[(size_t)i]) compare_lod(c, *a.physics[(size_t)i], *b.physics[(size_t)i], "yft.lod" + std::to_string(i));
    }
    compare_opt_frag_drawable(c, a.drawable_cloth, b.drawable_cloth, "yft.drawable_cloth");
    c.eq(a.vehicle_glass_windows, b.vehicle_glass_windows, "yft.vehicle_glass_windows");
    Drawable la, lb;
    la.lights = a.lights; lb.lights = b.lights;
    compare_drawable(c, la, lb, "yft");
    c.eq(a.cloths.size(), b.cloths.size(), "yft.cloths.count");
    for (size_t i = 0; i < a.cloths.size() && i < b.cloths.size(); i++) compare_environment_cloth(c, a.cloths[i], b.cloths[i], "yft.cloth[" + std::to_string(i) + "]");
    c.eq(a.unkB0, b.unkB0, "yft.unkB0"); c.eq(a.unkB8, b.unkB8, "yft.unkB8"); c.eq(a.unkBC, b.unkBC, "yft.unkBC"); c.eq(a.unkC0, b.unkC0, "yft.unkC0");
    c.eq(a.unkC4, b.unkC4, "yft.unkC4"); eq_mem(c, a.unkCC, b.unkCC, "yft.unkCC"); eq_mem(c, a.gravity_factor, b.gravity_factor, "yft.gravity");
    eq_mem(c, a.buoyancy_factor, b.buoyancy_factor, "yft.buoyancy");
}

// A fragment's cloth draws through its drawable_cloth when it has one, else through its drawable.
static void check_cloth_drawables(Cmp& c, const Bytes& file, const std::string& p) {
    rsc::Resource res = rsc::Resource::load(file);
    Reader r(res);
    const uint64_t root = rsc::kSystemBase;
    const uint64_t want = r.u64(root + 0xF8) ? r.u64(root + 0xF8) : r.u64(root + 0x30);
    for (uint64_t cp : r.array<uint64_t>(r.u64(root + 0x60), r.u16(root + 0x68))) {
        uint64_t drawable = 0;
        read_environment_cloth(r, cp, &drawable);
        if (drawable != want) c.fail(p + ": a cloth draws through neither drawable_cloth nor drawable");
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

// Enhanced drawables: walks the original and the written file side by side and compares, byte for byte, every structure
// whose layout is Enhanced's own (shaders, their parameter blocks and tables, texture stubs, textures, vertex and index
// buffers, declarations, buffer views, vertex and index data); pointers are compared as null / not null, vtables not at all.
struct Gen9Walk {
    const Reader& a; const Reader& b; Cmp& c;
    std::string where;
    void bytes(uint64_t pa, uint64_t pb, size_t n, std::initializer_list<size_t> ptrs, std::initializer_list<size_t> skip4, const std::string& what) {
        const uint8_t* x = a.resource().at(pa, n); const uint8_t* y = b.resource().at(pb, n);
        std::vector<bool> mask(n, false);
        for (size_t o : ptrs) {
            uint64_t u, v; std::memcpy(&u, x + o, 8); std::memcpy(&v, y + o, 8);
            if ((u == 0) != (v == 0)) c.fail(where + what + ": pointer at " + std::to_string(o) + " null in one file only");
            for (size_t k = 0; k < 8; k++) mask[o + k] = true;
        }
        for (size_t o : skip4) for (size_t k = 0; k < 4; k++) mask[o + k] = true;
        for (size_t i = 0; i < n; i++) if (!mask[i] && x[i] != y[i]) { c.fail(where + what + ": byte " + std::to_string(i) + " differs"); return; }
    }
    void view(uint64_t pa, uint64_t pb, const std::string& what) { if (pa && pb) bytes(pa, pb, 32, {0}, {}, what + ".view"); }
    void texture_ref(uint64_t pa, uint64_t pb) { if (pa && pb) bytes(pa, pb, 80, {0x28, 0x30, 0x38}, {0}, "texture stub"); }
    void texture(uint64_t pa, uint64_t pb) {
        bytes(pa, pb, 128, {0x28, 0x30, 0x38, 0x58}, {0}, "texture");
        const uint64_t n = (uint64_t)a.u32(pa + 8) * a.u32(pa + 12);
        if (n) bytes(a.u64(pa + 0x38), b.u64(pb + 0x38), (size_t)n, {}, {}, "texture data");
    }
    void txd(uint64_t pa, uint64_t pb) {
        if (!pa || !pb) return;
        const uint16_t n = a.u16(pa + 0x38);
        if (n != b.u16(pb + 0x38)) { c.fail(where + "txd count"); return; }
        for (uint16_t i = 0; i < n; i++) texture(a.u64(a.u64(pa + 0x30) + 8ull * i), b.u64(b.u64(pb + 0x30) + 8ull * i));
    }
    void shader(uint64_t pa, uint64_t pb) {
        bytes(pa, pb, 64, {8, 0x10, 0x18, 0x20}, {}, "shader");
        const uint64_t ia = a.u64(pa + 0x20), ib = b.u64(pb + 0x20);
        if (!ia || !ib) return;
        const uint8_t np = a.u8(ia + 4);
        bytes(ia, ib, 8 + 8ull * np, {}, {}, "shader table");
        const uint8_t nb = a.u8(ia), nt = a.u8(ia + 1), nu = a.u8(ia + 2), ns = a.u8(ia + 3), copies = a.u8(ia + 7);
        const uint64_t qa = a.u64(pa + 8), qb = b.u64(pb + 8);
        const uint16_t total = a.u16(pa + 0x3A);
        std::vector<size_t> ptrs;
        for (size_t i = 0; i < (size_t)nb * copies; i++) ptrs.push_back(i * 8);
        const size_t tex_at = (size_t)(a.u64(pa + 0x10) - qa);
        for (size_t i = 0; i < (size_t)nt * copies; i++) ptrs.push_back(tex_at + i * 8);
        const uint8_t* x = a.resource().at(qa, total); const uint8_t* y = b.resource().at(qb, total);
        std::vector<bool> mask(total, false);
        for (size_t o : ptrs) {
            uint64_t u, v; std::memcpy(&u, x + o, 8); std::memcpy(&v, y + o, 8);
            if ((u == 0) != (v == 0)) c.fail(where + "shader block: pointer at " + std::to_string(o) + " null in one file only");
            if (u && o < (size_t)nb * copies * 8 && u - qa != v - qb) c.fail(where + "shader block: buffer pointer at " + std::to_string(o) + " points elsewhere");
            for (size_t k = 0; k < 8; k++) mask[o + k] = true;
        }
        for (size_t i = 0; i < total; i++) if (!mask[i] && x[i] != y[i]) { c.fail(where + "shader block: byte " + std::to_string(i) + " of " + std::to_string(total) + " differs"); break; }
        for (size_t i = 0; i < nt; i++) texture_ref(a.u64(qa + tex_at + 8 * i), b.u64(qb + tex_at + 8 * i));
        (void)nu; (void)ns;
    }
    void geometry(uint64_t pa, uint64_t pb) {
        const uint64_t va = a.u64(pa + 0x18), vb = b.u64(pb + 0x18), xa = a.u64(pa + 0x38), xb = b.u64(pb + 0x38);
        if (va && vb) {
            bytes(va, vb, 64, {0x18, 0x30, 0x38}, {0}, "vertex buffer");
            view(a.u64(va + 0x30), b.u64(vb + 0x30), "vertex buffer");
            if (a.u64(va + 0x38) && b.u64(vb + 0x38)) bytes(a.u64(va + 0x38), b.u64(vb + 0x38), 320, {}, {}, "declaration");
            const size_t n = (size_t)a.u32(va + 8) * a.u16(va + 12);
            if (n) bytes(a.u64(va + 0x18), b.u64(vb + 0x18), n, {}, {}, "vertices");
        }
        if (xa && xb) {
            bytes(xa, xb, 64, {0x18, 0x30}, {0}, "index buffer");
            view(a.u64(xa + 0x30), b.u64(xb + 0x30), "index buffer");
            const size_t n = (size_t)a.u32(xa + 8) * 2;
            if (n) bytes(a.u64(xa + 0x18), b.u64(xb + 0x18), n, {}, {}, "indices");
        }
    }
    void drawable(uint64_t pa, uint64_t pb) {
        if (!pa || !pb) return;
        const uint64_t sa = a.u64(pa + 0x10), sb = b.u64(pb + 0x10);
        if (sa && sb) {
            bytes(sa, sb, 64, {8, 0x10}, {0}, "shader group");
            txd(a.u64(sa + 8), b.u64(sb + 8));
            const uint16_t n = a.u16(sa + 0x18);
            for (uint16_t i = 0; i < n; i++) shader(a.u64(a.u64(sa + 0x10) + 8ull * i), b.u64(b.u64(sb + 0x10) + 8ull * i));
        }
        for (int l = 0; l < 4; l++) {
            const uint64_t la = a.u64(pa + 0x50 + 8ull * l), lb = b.u64(pb + 0x50 + 8ull * l);
            if (!la || !lb) continue;
            const uint16_t mc = a.u16(la + 8);
            for (uint16_t m = 0; m < mc; m++) {
                const uint64_t ma = a.u64(a.u64(la) + 8ull * m), mb = b.u64(b.u64(lb) + 8ull * m);
                const uint16_t gc = a.u16(ma + 0x10);
                for (uint16_t k = 0; k < gc; k++) geometry(a.u64(a.u64(ma + 8) + 8ull * k), b.u64(b.u64(mb + 8) + 8ull * k));
            }
        }
    }
};

static void check_gen9_bytes(Cmp& c, const std::string& ext, const Bytes& original, const Bytes& written) {
    rsc::Resource ra = rsc::Resource::load(original), rb = rsc::Resource::load(written);
    Reader a(ra, Game::Enhanced), b(rb, Game::Enhanced);
    Gen9Walk w{a, b, c, ""};
    const uint64_t root = rsc::kSystemBase;
    if (ext == ".ytd") w.txd(root, root);
    else if (ext == ".ydr") w.drawable(root, root);
    else if (ext == ".ydd") {
        const uint16_t n = a.u16(root + 0x38);
        if (n != b.u16(root + 0x38)) { c.fail("ydd count"); return; }
        // the writer sorts by hash: match the drawables by their hash
        for (uint16_t i = 0; i < n; i++) {
            const uint32_t h = a.u32(a.u64(root + 0x20) + 4ull * i);
            for (uint16_t k = 0; k < n; k++)
                if (b.u32(b.u64(root + 0x20) + 4ull * k) == h) { w.where = hash_name(h) + ": "; w.drawable(a.u64(a.u64(root + 0x30) + 8ull * i), b.u64(b.u64(root + 0x30) + 8ull * k)); break; }
        }
    } else if (ext == ".ypt") {
        w.txd(a.u64(root + 0x20), b.u64(root + 0x20));
        const uint64_t da = a.u64(root + 0x30), db = b.u64(root + 0x30);
        for (uint16_t i = 0; da && db && i < a.u16(da + 0x38); i++) { w.where = "drawable " + std::to_string(i) + ": "; w.drawable(a.u64(a.u64(da + 0x30) + 8ull * i), b.u64(b.u64(db + 0x30) + 8ull * i)); }
    } else if (ext == ".yft") {
        for (size_t at : {0x30, 0xF8}) { w.where = at == 0x30 ? "drawable: " : "cloth drawable: "; w.drawable(a.u64(root + at), b.u64(root + at)); }
        const uint64_t aa = a.u64(root + 0x38), ab = b.u64(root + 0x38);
        for (uint32_t i = 0; aa && ab && i < a.u32(root + 0x48); i++) { w.where = "array " + std::to_string(i) + ": "; w.drawable(a.u64(aa + 8ull * i), b.u64(ab + 8ull * i)); }
        w.where = "";
        const uint64_t ga = a.u64(root + 0xE0), gb = b.u64(root + 0xE0);
        for (uint8_t i = 0; ga && gb && i < a.u8(root + 0xD9); i++) w.bytes(a.u64(ga + 8ull * i), b.u64(gb + 8ull * i), 416, {}, {}, "glass window");
        // the physics children's drawables
        const uint64_t pa = a.u64(root + 0xF0), pb = b.u64(root + 0xF0);
        for (int l = 0; pa && pb && l < 3; l++) {
            const uint64_t la = a.u64(pa + 0x10 + 8ull * l), lb = b.u64(pb + 0x10 + 8ull * l);
            if (!la || !lb) continue;
            const uint8_t n = a.u8(la + 0x11D);
            for (uint8_t i = 0; i < n; i++) {
                const uint64_t ca = a.u64(a.u64(la + 0xD0) + 8ull * i), cb = b.u64(b.u64(lb + 0xD0) + 8ull * i);
                for (size_t at : {0xA0, 0xA8}) { w.where = "lod" + std::to_string(l) + " child " + std::to_string(i) + ": "; w.drawable(a.u64(ca + at), b.u64(cb + at)); }
            }
        }
    }
}

// Particle rules: the two graphs walked side by side; every block's type and bytes, every link (where it is, where it
// leads, at which offset), texture and drawable link; a block shared in one must be shared the same way in the other.
static void compare_ptx_block(Cmp& c, const std::shared_ptr<PtxBlock>& a, const std::shared_ptr<PtxBlock>& b, std::map<const PtxBlock*, const PtxBlock*>& pairs, const std::string& p) {
    if ((bool)a != (bool)b) { c.fail(p + ": present in one only"); return; }
    if (!a) return;
    auto it = pairs.find(a.get());
    if (it != pairs.end()) { if (it->second != b.get()) c.fail(p + ": shared differently"); return; }
    pairs[a.get()] = b.get();
    if (a->type != b->type) { c.fail(p + ": type"); return; }
    if (a->type == PtxType::Root) return;
    if (a->bytes != b->bytes) { c.fail(p + ": bytes (type " + std::to_string((int)a->type) + ")"); return; }
    if (a->links.size() != b->links.size()) { c.fail(p + ": link count"); return; }
    for (size_t i = 0; i < a->links.size(); i++) {
        const auto& x = a->links[i]; const auto& y = b->links[i];
        if (x.at != y.at || x.offset != y.offset) { c.fail(p + ": link " + std::to_string(i)); return; }
        compare_ptx_block(c, x.to, y.to, pairs, p + "/" + std::to_string(x.at));
    }
    if (a->textures.size() != b->textures.size()) { c.fail(p + ": texture links"); return; }
    for (size_t i = 0; i < a->textures.size(); i++) if (a->textures[i].at != b->textures[i].at || a->textures[i].name != b->textures[i].name) c.fail(p + ": texture link");
    if (a->drawables.size() != b->drawables.size()) { c.fail(p + ": drawable links"); return; }
    for (size_t i = 0; i < a->drawables.size(); i++) if (a->drawables[i].at != b->drawables[i].at || a->drawables[i].index != b->drawables[i].index) c.fail(p + ": drawable link");
}

static void compare_ypt(Cmp& c, const ParticleDictionary& a, const ParticleDictionary& b) {
    c.eq(a.name, b.name, "ypt.name");
    c.eq(a.textures.has_value(), b.textures.has_value(), "ypt.has_textures");
    if (a.textures && b.textures) compare_txd(c, *a.textures, *b.textures, "ypt.txd");
    c.eq(a.drawable_hashes, b.drawable_hashes, "ypt.drawable_hashes");
    c.eq(a.drawables.size(), b.drawables.size(), "ypt.drawables.count");
    for (size_t i = 0; i < a.drawables.size() && i < b.drawables.size(); i++) compare_drawable(c, a.drawables[i], b.drawables[i], "ypt.drawable[" + std::to_string(i) + "]");
    for (int k = 0; k < 4; k++) c.eq(a.root_unk[k], b.root_unk[k], "ypt.root_unk");
    std::map<const PtxBlock*, const PtxBlock*> pairs;
    const PtxRuleList* la[] = {&a.particle_rules, &a.effect_rules, &a.emitter_rules};
    const PtxRuleList* lb[] = {&b.particle_rules, &b.effect_rules, &b.emitter_rules};
    for (int k = 0; k < 3; k++) {
        c.eq(la[k]->hashes, lb[k]->hashes, "ypt.rules.hashes");
        c.eq(la[k]->rules.size(), lb[k]->rules.size(), "ypt.rules.count");
        compare_ptx_block(c, la[k]->dictionary, lb[k]->dictionary, pairs, "ypt.rules" + std::to_string(k));
    }
    c.eq(a.blocks().size(), b.blocks().size(), "ypt.blocks.count");
}

// Navmeshes: every stored value bit for bit, and the written header's bytes (pointers aside) the original's.
static void compare_sector(Cmp& c, const NavSector& a, const NavSector& b, const std::string& p) {
    if (std::memcmp(&a.min, &b.min, 16) || std::memcmp(&a.max, &b.max, 16) || std::memcmp(&a.box, &b.box, sizeof a.box)) c.fail(p + ".box");
    c.eq(a.has_data, b.has_data, p + ".has_data"); c.eq(a.points_start, b.points_start, p + ".points_start"); c.eq(a.polys, b.polys, p + ".polys");
    eq_bits(c, a.points, b.points, p + ".points");
    for (size_t i = 0; i < 4; i++) {
        c.eq((bool)a.children[i], (bool)b.children[i], p + ".child");
        if (a.children[i] && b.children[i]) compare_sector(c, *a.children[i], *b.children[i], p + "." + std::to_string(i));
    }
}
static void compare_ynv(Cmp& c, const NavMesh& a, const NavMesh& b, const Bytes& original, const Bytes& written) {
    c.eq(a.content_flags, b.content_flags, "ynv.flags"); c.eq(a.version, b.version, "ynv.version"); c.eq(a.build, b.build, "ynv.build");
    c.eq(a.area_id, b.area_id, "ynv.area"); c.eq(a.adjacent_areas, b.adjacent_areas, "ynv.adjacent");
    if (std::memcmp(a.transform.data(), b.transform.data(), 64) || std::memcmp(&a.size, &b.size, 12) || a.size_w != b.size_w) c.fail("ynv.transform/size");
    eq_bits(c, a.vertices, b.vertices, "ynv.vertices"); c.eq(a.indices, b.indices, "ynv.indices"); eq_bits(c, a.edges, b.edges, "ynv.edges");
    eq_bits(c, a.polys, b.polys, "ynv.polys"); eq_bits(c, a.portals, b.portals, "ynv.portals"); c.eq(a.portal_links, b.portal_links, "ynv.portal_links");
    c.eq(a.sectors.has_value(), b.sectors.has_value(), "ynv.sectors");
    if (a.sectors && b.sectors) compare_sector(c, *a.sectors, *b.sectors, "ynv.sector");
    rsc::Resource ra = rsc::Resource::load(original), rb = rsc::Resource::load(written);
    const uint8_t* x = ra.system.data() + 0x10; const uint8_t* y = rb.system.data() + 0x10;
    for (size_t o = 0x10; o < 368; o++) {
        const bool ptr = (o >= 0x70 && o < 0x78) || (o >= 0x80 && o < 0x90) || (o >= 0x118 && o < 0x138);
        if (!ptr && x[o - 0x10] != y[o - 0x10]) { c.fail("ynv: header byte " + std::to_string(o) + " differs"); break; }
    }
}

static bool g_cross = false;   // --cross: also write every ytd/ydr/ydd/yft in the other edition, read it and compare

// The edition of a resource file, from its version.
static Game game_of_file(const std::string& ext, const Bytes& file) {
    if (!rsc::Resource::is_resource(file)) return Game::Legacy;
    int v = 0;
    std::memcpy(&v, file.data() + 4, 4);
    return rsc::game_of(ext, v);
}
static Game other(Game g) { return g == Game::Enhanced ? Game::Legacy : Game::Enhanced; }

static bool test_bytes(const std::string& label, const std::string& ext, const Bytes& file) {
    g_digest = 0;
    const Game game = game_of_file(ext, file);
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
            auto a = read_ytd(file); Bytes w = write_ytd(a, game); mix(w); out_size = w.size(); auto b = read_ytd(w); compare_txd(c, a, b, "txd");
            if (rsc::game_of(ext, rsc::Resource::load(w).version) != game) c.fail("ytd: written version");
            if (game == Game::Enhanced) check_gen9_bytes(c, ext, file, w);
            if (g_cross) { Cmp x; x.cross = true; compare_txd(x, a, read_ytd(write_ytd(a, other(game))), "cross.txd"); if (x.diffs) c.fail(x.first_diff); }
        } else if (ext == ".ydr") {
            auto a = read_ydr(file); Bytes w = write_ydr(a, game); mix(w); out_size = w.size(); auto b = read_ydr(w); compare_drawable(c, a, b, "ydr");
            if (rsc::game_of(ext, rsc::Resource::load(w).version) != game) c.fail("ydr: written version");
            if (game == Game::Enhanced) check_gen9_bytes(c, ext, file, w);
            if (g_cross) { Cmp x; x.cross = true; compare_drawable(x, a, read_ydr(write_ydr(a, other(game))), "cross.ydr"); if (x.diffs) c.fail(x.first_diff); }
        } else if (ext == ".ybn") {
            auto a = read_ybn(file); Bytes w = write_ybn(*a); mix(w); out_size = w.size(); auto b = read_ybn(w); compare_bound(c, *a, *b, "ybn");
        } else if (ext == ".ydd") {
            auto a = read_ydd(file); Bytes w = write_ydd(a, game); mix(w); out_size = w.size(); auto b = read_ydd(w);
            if (rsc::game_of(ext, rsc::Resource::load(w).version) != game) c.fail("ydd: written version");
            if (game == Game::Enhanced) check_gen9_bytes(c, ext, file, w);
            c.eq(a.drawables.size(), b.drawables.size(), "ydd.count");
            // the writer sorts by hash (stable), so sort the input the same way and match by position
            std::vector<size_t> order(a.drawables.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return a.hashes[x] < a.hashes[y]; });
            for (size_t i = 0; i < order.size() && i < b.drawables.size(); i++) {
                c.eq(a.hashes[order[i]], b.hashes[i], "ydd.hash");
                compare_drawable(c, a.drawables[order[i]], b.drawables[i], "ydd[" + hash_name(a.hashes[order[i]]) + "]");
            }
        } else if (ext == ".ypt") {
            size_t census = 0;
            std::string where;
            const ParticleDictionary a = read_ypt_checked(file, &census, &where);
            if (census) c.fail("ypt: " + std::to_string(census) + " pointer-like values the schema does not know:" + where);
            Bytes w = write_ypt(a, game); mix(w); out_size = w.size();
            if (rsc::game_of(ext, rsc::Resource::load(w).version) != game) c.fail("ypt: written version");
            compare_ypt(c, a, read_ypt(w));
            if (game == Game::Enhanced) check_gen9_bytes(c, ext, file, w);
            if (g_cross) { Cmp x; x.cross = true; compare_ypt(x, a, read_ypt(write_ypt(a, other(game)))); if (x.diffs) c.fail("cross: " + x.first_diff); }
        } else if (ext == ".ynv") {
            const NavMesh a = read_ynv(file);
            Bytes w = write_ynv(a); mix(w); out_size = w.size();
            compare_ynv(c, a, read_ynv(w), file, w);
        } else if (ext == ".yld") {
            auto a = read_yld(file); Bytes w = write_yld(a); mix(w); out_size = w.size();
            if (rsc::Resource::load(w).version != rsc::kYld) c.fail("yld: written version");
            auto b = read_yld(w);
            c.eq(a.cloths.size(), b.cloths.size(), "yld.count");
            std::vector<size_t> order(a.cloths.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return a.hashes[x] < a.hashes[y]; });
            for (size_t i = 0; i < order.size() && i < b.cloths.size(); i++) {
                c.eq(a.hashes[order[i]], b.hashes[i], "yld.hash");
                compare_character_cloth(c, a.cloths[order[i]], b.cloths[i], "yld[" + hash_name(a.hashes[order[i]]) + "]");
            }
        } else if (ext == ".yft") {
            const Fragment a = read_yft(file);
            Bytes w = write_yft(a, game); mix(w); out_size = w.size();
            if (rsc::Resource::load(w).version != rsc::version_for("yft", game)) c.fail("yft: written version");
            compare_fragment(c, a, read_yft(w));
            if (game == Game::Enhanced) check_gen9_bytes(c, ext, file, w);
            if (g_cross) { Cmp x; x.cross = true; compare_fragment(x, a, read_yft(write_yft(a, other(game)))); if (x.diffs) c.fail("cross: " + x.first_diff); }
            check_cloth_drawables(c, file, "original");
            check_cloth_drawables(c, w, "written");
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
           ext == ".meta" || ext == ".xml" || ext == ".yld" || ext == ".yft" || ext == ".ypt" || ext == ".ynv";
}

// A file test_bytes had nothing to test in is not counted.
static bool counted() { bool c = !g_untested; g_untested = false; return c; }

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
        else if (a == "--cross") g_cross = true;
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
            if (!counted()) total--;
            note_digest(label);
        }
    }
    for (const fs::path& p : paths) {
        if (fs::is_directory(p)) {
            for (auto& e : fs::recursive_directory_iterator(p)) {
                if (!e.is_regular_file() || !wanted(lower(e.path().extension().string()))) continue;
                total++;
                if (!test_file(e.path())) failed++;
                if (!counted()) total--;
                note_digest(e.path().filename().string());
            }
        } else { total++; if (!test_file(p)) failed++; if (!counted()) total--; note_digest(p.filename().string()); }
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
