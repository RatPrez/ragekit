// occluder_test: box occluders, occlude models and removing entities (occluder.h).
//   - a box occluder encodes and decodes (centre and size to a quarter metre; the length runs along (sin, cos))
//   - oriented boxes and triangles against boxes
//   - entities removed from a ymap and the parent indices of a child ymap renumbered
//   - with the game installed: every occlusion ymap round-trips
// Exit 0 = every check passed.
#include <cmath>
#include <cstdio>
#include <cstring>
#include "ragekit/gameindex.h"
#include "ragekit/hash.h"
#include "ragekit/occluder.h"
#include "ragekit/world.h"

using namespace rk;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("%-86s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }


static OccludeMesh wall(float y0, float x0, float x1, float z0, float z1) {
    OccludeMesh m;
    m.vertices = {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}};
    m.indices = {0, 2, 1, 0, 3, 2};   // (b - a) x (c - a) points to -y
    return m;
}

static OrientedBox box(Vec3 centre, Vec3 half, float yaw_deg = 0) {
    OrientedBox b;
    b.centre = centre;
    b.half = half;
    const float c = std::cos(yaw_deg * 3.14159265f / 180), s = std::sin(yaw_deg * 3.14159265f / 180);
    b.axis[0] = {c, s, 0};
    b.axis[1] = {-s, c, 0};
    b.axis[2] = {0, 0, 1};
    return b;
}


int main() {
    // ---- the box occluder's encoding
    {
        OccluderBox o;
        o.centre = {-836.25f, 123.f, 52.f};
        o.size = {5.75f, 13.5f, 13.f};
        o.dir_x = 0.f;
        o.dir_y = 1.f;   // the length runs along +y
        const BoxOccluder b = encode_box_occluder(o);
        check(b.center_x == -3345 && b.center_y == 492 && b.center_z == 208 && b.length == 23 && b.width == 54 && b.height == 52, "box occluder: centre and size in quarter metres");
        check(b.sin_z == 0 && b.cos_z == 16384, "box occluder: a length along +y is (sin 0, cos 16384)");
        const OccluderBox r = decode_box_occluder(b);
        check(std::fabs(r.centre.x - o.centre.x) < 1e-4f && std::fabs(r.size.y - 13.5f) < 1e-4f && std::fabs(r.dir_y - 1.f) < 1e-5f && std::fabs(r.dir_x) < 1e-5f, "box occluder: decode(encode(x)) = x");
        // a file's box: cos 16331, sin -1313 (the length runs along (-0.080, 0.997): nearly north)
        BoxOccluder f;
        f.cos_z = 16331; f.sin_z = -1313; f.length = 23; f.width = 54;
        const OccluderBox fb = decode_box_occluder(f);
        check(fb.dir_x < -0.07f && fb.dir_x > -0.09f && fb.dir_y > 0.99f, "box occluder: (iSinZ, iCosZ) is the length's direction");
        const OrientedBox ob = fb.box();
        check(std::fabs(dot(ob.axis[0], ob.axis[1])) < 1e-5f && std::fabs(ob.axis[2].z - 1.f) < 1e-6f, "box occluder: orthonormal axes");
    }
    // ---- boxes and triangles
    {
        check(boxes_overlap(box({0, 0, 0}, {1, 1, 1}), box({1.5f, 0, 0}, {1, 1, 1})), "boxes: two overlapping");
        check(!boxes_overlap(box({0, 0, 0}, {1, 1, 1}), box({2.5f, 0, 0}, {1, 1, 1})), "boxes: two apart");
        check(boxes_overlap(box({0, 0, 0}, {1, 1, 1}), box({2.0f, 0, 0}, {1, 1, 1}), 0.01f), "boxes: touching counts with a margin");
        check(!boxes_overlap(box({0, 0, 0}, {1, 1, 1}), box({2.4f, 0, 0}, {1, 1, 1}, 45.f)) == !boxes_overlap(box({2.4f, 0, 0}, {1, 1, 1}, 45.f), box({0, 0, 0}, {1, 1, 1})), "boxes: symmetric");
        check(boxes_overlap(box({0, 0, 0}, {1, 1, 1}), box({2.3f, 0, 0}, {1, 1, 1}, 45.f)), "boxes: a corner of a turned box reaches in (1.41 + 1 > 2.3 - 1)");
        check(!boxes_overlap(box({0, 0, 0}, {1, 1, 1}), box({2.6f, 0, 0}, {1, 1, 1}, 45.f)), "boxes: the turned box's corner stops short");
        const OrientedBox unit = box({0, 0, 0}, {1, 1, 1});
        check(triangle_overlaps_box({-3, 0, 0}, {3, 0.2f, 0}, {0, 3, 0}, unit), "triangle: crossing the box");
        check(triangle_overlaps_box({-0.2f, -0.2f, -0.2f}, {0.2f, -0.2f, -0.2f}, {0, 0.2f, 0.2f}, unit), "triangle: wholly inside");
        check(!triangle_overlaps_box({2, 2, 0}, {3, 2, 0}, {2, 3, 0}, unit), "triangle: outside");
        check(!triangle_overlaps_box({-3, 1.5f, 0}, {3, 1.5f, 0}, {0, 4, 0}, unit), "triangle: above the box, parallel to a face");
        check(triangle_overlaps_box({-3, 1, 0}, {3, 1, 0}, {0, 4, 0}, unit), "triangle: lying on a face counts as touching");
    }
    // ---- entities
    {
        MapData parent, child;
        for (int i = 0; i < 5; i++) { Entity e; e.archetype_name = (uint32_t)(100 + i); e.position = {(float)i, 0, 0}; parent.entities.push_back(e); }
        for (int i = 0; i < 5; i++) { Entity e; e.archetype_name = (uint32_t)(200 + i); e.parent_index = i; child.entities.push_back(e); }
        child.entities.push_back([] { Entity e; e.parent_index = -1; return e; }());
        const std::vector<int32_t> remap = remove_entities(parent, {3, 1, 3});
        check(parent.entities.size() == 3 && parent.entities[0].archetype_name == 100 && parent.entities[1].archetype_name == 102 && parent.entities[2].archetype_name == 104, "entities: 1 and 3 removed, the order kept");
        check(remap.size() == 5 && remap[0] == 0 && remap[1] == -1 && remap[2] == 1 && remap[3] == -1 && remap[4] == 2, "entities: the old -> new map");
        const size_t n = remap_parent_indices(child, remap);
        check(n == 4 && child.entities[0].parent_index == 0 && child.entities[1].parent_index == -1 && child.entities[2].parent_index == 1 && child.entities[3].parent_index == -1 &&
              child.entities[4].parent_index == 2 && child.entities[5].parent_index == -1, "entities: the children's parent indices follow (4 changed)");
        const MapData back = read_ymap(write_ymap(parent));
        check(back.entities.size() == 3 && back.entities[2].archetype_name == 104, "entities: the shorter list writes and reads back");
    }
    // ---- the game's own occlusion files
    {
        const std::string game = GameIndex::default_game_dir();
        if (game.empty()) {
            std::printf("no game install: the checks on the game's files are skipped\n");
        } else {
            GameIndex gi = GameIndex::open(game, {});
            WorldIndex w = WorldIndex::open(gi, {});
            size_t files = 0, same = 0, models = 0, boxes = 0;
            for (size_t i = 0; i < w.maps().size(); i++) {
                const WorldMap& m = w.maps()[i];
                if (!(m.content_flags & 32) || !m.readable) continue;
                std::shared_ptr<const MapData> d;
                try { d = w.map_data((uint32_t)i); } catch (const std::exception&) { continue; }
                if (d->box_occluders.empty() && d->occlude_models.empty()) continue;
                files++;
                const MapData back = read_ymap(write_ymap(*d));
                bool ok = back.box_occluders == d->box_occluders && back.occlude_models.size() == d->occlude_models.size();
                for (size_t k = 0; ok && k < d->occlude_models.size(); k++) {
                    const OccludeMesh a = read_occlude_model(d->occlude_models[k]), b = read_occlude_model(back.occlude_models[k]);
                    ok = a.vertices.size() == b.vertices.size() && a.indices == b.indices && a.flags == b.flags;
                    for (size_t v = 0; ok && v < a.vertices.size(); v++) ok = a.vertices[v].x == b.vertices[v].x && a.vertices[v].y == b.vertices[v].y && a.vertices[v].z == b.vertices[v].z;
                }
                same += ok;
                models += d->occlude_models.size();
                boxes += d->box_occluders.size();
            }
            char buf[160];
            std::snprintf(buf, sizeof buf, "game: %zu occlusion ymaps (%zu box occluders, %zu models) write and read back unchanged", files, boxes, models);
            check(files > 100 && same == files, buf);
        }
    }
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
