// rk_gfx_test: Scaleform movies made from scratch (gfx_build.h) written, read back and compared; with a GTA V install
// (GameIndex::default_game_dir(), or the folder given as the argument) every shape, sprite, PlaceObject, frame label
// and ExporterInfo of every .gfx is read into its fields and written back, and must come back byte for byte.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include "ragekit/ragekit.h"
#include "ragekit/gfx_build.h"

using namespace rk;
using namespace rk::gfx;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

static double area_of(const Shape& s, Rgba c) {
    for (const auto& [col, a] : shape_fill_areas(s)) if (col == c) return a;
    return 0;
}

static void built_movie() {
    // a floor plan as the game draws its interiors: a dark outline, a light floor over it, an exit strip, one curve
    ShapeBuilder b(1, 3);
    const uint32_t wall = b.fill({67, 67, 67});
    b.polygon({{-2000, -1000}, {2000, -1000}, {2000, 1000}, {-2000, 1000}}, wall);
    b.layer();
    const uint32_t floor = b.fill({150, 150, 150});
    b.polygon({{-1960, 960}, {1960, 960}, {1960, -960}, {-1960, -960}}, floor);   // the other way round
    b.layer();
    const uint32_t exit = b.fill({255, 255, 255});
    const uint32_t outline = b.line(20, {0, 0, 0, 128});
    b.polygon({{-200, 960}, {200, 960}, {200, 1000}, {-200, 1000}}, exit, outline);
    b.polyline({{0, 0}, {300, 0}, {300, 50000}}, outline);   // a long edge (17-bit deltas)
    Shape s = b.finish();
    ShapeRecord curve;
    curve.kind = ShapeRecord::Curve;
    curve.cdx = 100; curve.cdy = -100; curve.dx = 100; curve.dy = 100;
    s.records.push_back(curve);
    check(std::abs(area_of(s, {67, 67, 67}) - 4000.0 * 2000) < 1, "the outline's area");
    check(std::abs(area_of(s, {150, 150, 150}) - 3920.0 * 1920) < 1, "a polygon given anticlockwise fills its inside");
    check(s.bounds.x_min == -2010 && s.bounds.y_max == 50010, "bounds widened by half the line width");

    GfxMovie m;
    m.frame = frame_header({0, 6000, 0, 6000, {}}, 30, 1);
    ExporterInfo ei;
    ei.swf_name = "test_minimap";
    m.tags.push_back(exporter_info_tag(ei));
    m.tags.push_back(file_attributes_tag(0));
    m.tags.push_back(background_tag({255, 255, 255}));
    m.tags.push_back(shape_tag(s));
    Sprite sp;
    sp.id = 2;
    Place inner;
    inner.depth = 1;
    inner.character = 1;
    inner.matrix = Matrix{};
    sp.tags = {place_tag(inner), show_frame_tag(), end_tag()};
    m.tags.push_back(sprite_tag(sp));
    Place level;
    level.depth = 3;
    level.character = 2;
    level.matrix = Matrix::make(0.1, 0.1, 0.5, 123456, -654321);
    ColourTransform cx;
    cx.has_mul = true;
    cx.mul[3] = 0;
    level.colour = cx;
    level.ratio = 1;
    level.name = "level_0";
    m.tags.push_back(place_tag(level));
    Place p3;
    p3.place3 = true;
    p3.depth = 4;
    p3.character = 2;
    p3.blend_mode = 2;
    p3.class_name = "Floor";
    m.tags.push_back(place_tag(p3));
    m.tags.push_back(export_assets_tag({{2, "level_sprite"}}));
    m.tags.push_back(frame_label_tag("INT1"));
    m.tags.push_back(show_frame_tag());
    m.tags.push_back(end_tag());

    const Bytes file = write_gfx(m);
    const GfxMovie r = read_gfx(file);
    check(write_gfx(r) == file, "the movie comes back byte for byte");
    const Rect fr = frame_rect(r);
    check(fr == Rect{0, 6000, 0, 6000, {}} && r.frame_rate() == 30 && r.frame_count() == 1, "frame rectangle, rate and count");
    check(read_exporter_info(r.tags[0]).swf_name == "test_minimap", "ExporterInfo");
    const Shape back = read_shape(r.tags[3]);
    check(back == s, "the shape reads back field for field");
    const auto paths = shape_paths(back);
    check(paths.size() == 4 && paths[2].layer == 2 && paths[2].fill1_style && paths[2].fill1_style->colour == Rgba{255, 255, 255}, "paths with their layer and styles");
    check(paths.back().points.back().curve && paths.back().points.back().x == 500, "a curve's end in absolute twips");
    const Sprite sb = read_sprite(r.tags[4]);
    check(sb.id == 2 && sb.tags.size() == 3 && read_place(sb.tags[0]) == inner, "a sprite and the place inside it");
    const Place lb = read_place(r.tags[5]);
    check(lb == level && lb.name == "level_0" && lb.colour->mul[3] == 0, "PlaceObject2 with matrix, colour, ratio and name");
    double x, y;
    lb.matrix->apply(1000, 0, x, y);
    check(std::abs(x - (123456 + 100 * std::cos(0.5))) < 0.01 && std::abs(y - (-654321 + 100 * std::sin(0.5))) < 0.01, "the matrix maps a point");
    check(read_place(r.tags[6]) == p3, "PlaceObject3 with a class name and a blend mode");
    check(r.exports().size() == 1 && r.exports()[0] == "level_sprite", "ExportAssets");
    check(read_frame_label(r.tags[8]) == "INT1", "FrameLabel");
    const auto walk = all_tags(r);
    check(walk.tags.size() == r.tags.size() + 3 && walk.tags[5].sprite_id == 2, "all_tags walks into sprites");

    bool threw = false;
    try { ShapeRecord e; e.kind = ShapeRecord::Line; e.dx = 70000; Shape t; t.records = {e}; (void)shape_tag(t); } catch (const Error&) { threw = true; }
    check(threw, "an edge past 17 bits throws");
    threw = false;
    try { GfxTag t = shape_tag(s); t.payload.resize(t.payload.size() / 2); (void)read_shape(t); } catch (const Error&) { threw = true; }
    check(threw, "a cut shape throws");
}

static void game_movies(const std::string& dir) {
    GameIndex gi;
    try { gi = GameIndex::open(dir); } catch (const std::exception& e) { std::printf("skip  no game install (%s)\n", e.what()); return; }
    size_t movies = 0, shapes = 0, places = 0, sprites = 0, others = 0, interiors = 0, interior_shapes = 0;
    std::map<std::string, size_t> bad;
    std::string first_bad;
    for (const IndexedFile& f : gi.files()) {
        if (gi.extension(f) != ".gfx") continue;
        Bytes file;
        try { file = gi.extract(f); } catch (const std::exception&) { continue; }
        if (!is_gfx(file)) continue;
        const GfxMovie m = read_gfx(file);
        movies++;
        const std::string name = gi.file_name(f);
        const bool interior = name.rfind("int", 0) == 0 && name.size() > 7 && std::isdigit((unsigned char)name[3]);
        if (interior) interiors++;
        const TagWalk w = all_tags(m);
        for (const TagRef& t : w.tags) {
            const GfxTag& g = *t.tag;
            std::string kind;
            Bytes again;
            try {
                switch (g.code) {
                case kDefineShape: case kDefineShape2: case kDefineShape3: case kDefineShape4:
                    kind = "shape"; again = shape_tag(read_shape(g)).payload; shapes++; if (interior) interior_shapes++; break;
                case kPlaceObject2: case kPlaceObject3: kind = "place"; again = place_tag(read_place(g)).payload; places++; break;
                case kDefineSprite: kind = "sprite"; again = sprite_tag(read_sprite(g)).payload; sprites++; break;
                case kExporterInfo: kind = "exporter info"; again = exporter_info_tag(read_exporter_info(g)).payload; others++; break;
                case kFrameLabel: kind = "frame label"; again = frame_label_tag(read_frame_label(g)).payload; others++; break;
                default: continue;
                }
            } catch (const std::exception& e) {
                again = {};
            }
            if (again != g.payload) {
                if (std::getenv("RK_GFX_VERBOSE") && bad[kind] < 4) {
                    size_t at = 0;
                    while (at < again.size() && at < g.payload.size() && again[at] == g.payload[at]) at++;
                    std::printf("      %s %s: %zu / %zu bytes, first difference at %zu\n        game ", name.c_str(), kind.c_str(), g.payload.size(), again.size(), at);
                    for (size_t i = at > 4 ? at - 4 : 0; i < std::min(g.payload.size(), at + 12); i++) std::printf("%02x", g.payload[i]);
                    std::printf("\n        ours ");
                    for (size_t i = at > 4 ? at - 4 : 0; i < std::min(again.size(), at + 12); i++) std::printf("%02x", again[i]);
                    std::printf("\n");
                }
                if (!bad[kind]++ || first_bad.empty()) first_bad = gi.path(f) + " " + kind + " (tag " + std::to_string(g.code) + ")";
            }
        }
    }
    std::printf("      %zu movies (%zu interior minimaps with %zu shapes): %zu shapes, %zu places, %zu sprites, %zu labels / exporter infos\n",
                movies, interiors, interior_shapes, shapes, places, sprites, others);
    size_t nbad = 0;
    for (const auto& [k, n] : bad) { std::printf("      %zu %s tags differ\n", n, k.c_str()); nbad += n; }
    check(movies > 1000 && interiors > 280, "the install's movies and interior minimaps found");
    check(nbad == 0, "every shape, place, sprite, label and exporter info comes back byte for byte" + (first_bad.empty() ? "" : " (first: " + first_bad + ")"));
}

int main(int argc, char** argv) {
    built_movie();
    game_movies(argc > 1 ? argv[1] : GameIndex::default_game_dir());
    std::printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
