// rk_xml_test: checks ragekit's CodeWalker XML writers (xml.h) against CodeWalker itself.
//
//   rk_xml_test selftest
//       float printing and flag names against known .NET output (no files needed)
//   rk_xml_test gen <list.txt> <rk out dir> <cw dir>
//       writes ragekit's XML + DDS for every file of the list into <rk out dir> (the way `rk xml` does) and compares them
//       with what `oracle cwxmllist <list> <cw dir>` wrote: byte identical / semantically equal (same text, numbers within
//       1e-5 relative) / different. Also writes <rk out dir>/xmlin.txt, the list for `oracle xmlinlist`.
//   rk_xml_test summary <list.txt> <imported dir>
//       reads each original and CodeWalker's re-import of ragekit's XML (`oracle xmlinlist`, binaries named like the
//       originals) with ragekit and compares a summary: shaders and parameters, textures, models, vertex/index counts and
//       hashes, skeleton, lights, bound types / vertex / polygon / material counts.
//
// Exit code 0 when nothing differs semantically (identical + semantic only) / all summaries equal.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include "ragekit/ragekit.h"

namespace fs = std::filesystem;
using namespace rk;

static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string ext_of(const std::string& p) { return lower(fs::path(p).extension().string()); }

static std::string read_text(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "<missing>";
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static std::vector<std::string> read_lines(const std::string& p) {
    std::vector<std::string> out;
    std::ifstream f(p);
    for (std::string l; std::getline(f, l);) { while (!l.empty() && (l.back() == '\r')) l.pop_back(); if (!l.empty()) out.push_back(l); }
    return out;
}

// ---------------------------------------------------------------- text comparison

static std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) { out.push_back(cur); cur.clear(); } };
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '<' || c == '>' || c == '=' || c == '"' || c == ',' || c == '/') flush();
        else cur.push_back(c);
    }
    flush();
    return out;
}
static bool as_number(const std::string& t, double& v) {
    if (t.empty()) return false;
    char* e = nullptr;
    v = std::strtod(t.c_str(), &e);
    return e && *e == 0;
}

enum class Cmp { Identical, Semantic, Different };

static Cmp compare_text(const std::string& a, const std::string& b, std::string& why) {
    if (a == b) return Cmp::Identical;
    auto ta = tokens(a), tb = tokens(b);
    if (ta.size() != tb.size()) {
        why = "token count " + std::to_string(ta.size()) + " vs " + std::to_string(tb.size());
        // locate the first differing line for the report
    } else {
        for (size_t i = 0; i < ta.size(); i++) {
            if (ta[i] == tb[i]) continue;
            double x, y;
            if (as_number(ta[i], x) && as_number(tb[i], y)) {
                double m = std::max({1.0, std::fabs(x), std::fabs(y)});
                if (std::fabs(x - y) <= 1e-5 * m) continue;
            }
            why = "token " + std::to_string(i) + ": '" + ta[i] + "' vs '" + tb[i] + "'";
            goto diff;
        }
        return Cmp::Semantic;
    }
diff:
    std::stringstream sa(a), sb(b);
    std::string la, lb;
    for (int n = 1;; n++) {
        bool ga = (bool)std::getline(sa, la), gb = (bool)std::getline(sb, lb);
        if (!ga && !gb) break;
        if (la != lb || ga != gb) { why += " (line " + std::to_string(n) + ": '" + (ga ? la : "<end>") + "' vs '" + (gb ? lb : "<end>") + "')"; break; }
    }
    return Cmp::Different;
}

// ---------------------------------------------------------------- self test

static int selftest() {
    struct { uint32_t bits; const char* s; } f[] = {
        {0x00000000, "0"}, {0x80000000, "-0"}, {0x3F800000, "1"}, {0xBF800000, "-1"}, {0x3F000000, "0.5"}, {0x3727C5AC, "1E-05"},
        {0x38D1B717, "0.0001"}, {0x47C35000, "100000"}, {0x4CBEBC20, "100000000"}, {0x4E6E6B28, "1E+09"}, {0x4CEB79A3, "123456790"},
        {0x4B800000, "16777216"}, {0x5A0E1BCA, "1E+16"}, {0xFFC00000, "NaN"}, {0x7F800000, "Infinity"}, {0xFF800000, "-Infinity"},
        {0x00000001, "1E-45"}, {0x7F7FFFFF, "3.4028235E+38"}, {0x34210FB0, "1.5E-07"}, {0x43286666, "168.4"}, {0x3D4CCCCD, "0.05"}};
    int bad = 0;
    for (auto& t : f) {
        float v; std::memcpy(&v, &t.bits, 4);
        auto s = xml_float(v);
        if (s != t.s) { std::printf("float %08X: '%s', expected '%s'\n", t.bits, s.c_str(), t.s); bad++; }
    }
    std::printf("selftest: %d wrong\n", bad);
    return bad ? 1 : 0;
}

// `rk_xml_test floats <file>`: lines "<float bits in hex> <string .NET printed for it>"
static int cmd_floats(const std::string& path) {
    int n = 0, bad = 0;
    for (auto& l : read_lines(path)) {
        auto sp = l.find(' ');
        if (sp == std::string::npos) continue;
        uint32_t bits = (uint32_t)std::strtoul(l.substr(0, sp).c_str(), nullptr, 16);
        float v; std::memcpy(&v, &bits, 4);
        n++;
        auto s = xml_float(v);
        if (s != l.substr(sp + 1)) { if (bad++ < 20) std::printf("float %08X: '%s' expected '%s'\n", bits, s.c_str(), l.substr(sp + 1).c_str()); }
    }
    std::printf("floats: %d checked, %d wrong\n", n, bad);
    return bad ? 1 : 0;
}

// ---------------------------------------------------------------- synth

static Image gradient(uint32_t w, uint32_t h, int seed) {
    Image img; img.width = w; img.height = h; img.rgba.resize((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        auto* p = img.px(x, y);
        p[0] = (uint8_t)(x * 255 / w); p[1] = (uint8_t)(y * 255 / h); p[2] = (uint8_t)(seed * 40); p[3] = (uint8_t)(128 + 127 * ((x / 4 + y / 4) & 1));
    }
    return img;
}

static BoundPtr prim(BoundType t, Vec3 mn, Vec3 mx, float margin) {
    auto b = std::make_shared<Bound>();
    b->type = t;
    b->box_min = mn; b->box_max = mx; b->box_center = (mn + mx) * 0.5f; b->sphere_center = b->box_center;
    b->sphere_radius = (mx - mn).length() * 0.5f; b->margin = margin; b->volume = 1.5f; b->inertia = {0.1f, 0.2f, 0.3f};
    b->material_index = 3; b->procedural_id = 7; b->material_colour_index = 2; b->poly_flags = 5; b->unk_flags = 0;
    return b;
}

// Files built by ragekit itself (the fixed BoundTransform layout, bound primitives, lights, skeleton, array parameters ...).
static int cmd_synth(const std::string& dir) {
    fs::create_directories(dir);
    std::vector<Vec3> v = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}, {0, 0, 2}, {2, 0, 2}, {2, 2, 2}, {0, 2, 2}};
    std::vector<uint32_t> t = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 2, 3, 7, 2, 7, 6, 1, 2, 6, 1, 6, 5, 3, 0, 4, 3, 4, 7};
    BoundMaterial mat; mat.set_type(4); mat.set_room_id(3); mat.set_flags(kMatStairs | kMatSeeThrough | kMatNoDecal);
    auto bvh = Bound::make_mesh(v, t, mat, true);
    auto plain = Bound::make_mesh(v, t, mat, false);
    // a geometry with every polygon kind
    {
        auto& g = *plain->geometry;
        BoundMaterial m2; m2.set_type(9); m2.data2 = 0x00050200;
        g.materials.push_back(m2);
        g.material_colours = {0x01020304u, 0xFF8040A0u};
        g.polygons.push_back(BoundPolygon::sphere(0, 0.5f)); g.polygon_materials.push_back(1);
        g.polygons.push_back(BoundPolygon::capsule(0, 1, 0.25f)); g.polygon_materials.push_back(0);
        g.polygons.push_back(BoundPolygon::box(0, 1, 2, 4)); g.polygon_materials.push_back(1);
        g.polygons.push_back(BoundPolygon::cylinder(2, 3, 0.125f)); g.polygon_materials.push_back(0);
        plain->finalize();
    }
    Mat4 rot; rot.at(0, 0) = 0; rot.at(0, 1) = 1; rot.at(1, 0) = -1; rot.at(1, 1) = 0; rot.at(3, 0) = 5; rot.at(3, 1) = -2; rot.at(3, 2) = 1.5f;
    std::vector<BoundChild> kids(7);
    kids[0].bound = bvh; kids[0].flags1 = 0x0000FFFE; kids[0].flags2 = 0xFFFF0000u;
    kids[1].bound = plain; kids[1].transform = BoundTransform::from(rot); kids[1].flags1 = 1 << 13; kids[1].flags2 = 0x80000000u;
    kids[2].bound = prim(BoundType::Box, {-1, -2, -3}, {1, 2, 3}, 0.04f); kids[2].transform.col[3] = {10, 0, 0};
    kids[3].bound = prim(BoundType::Sphere, {-1, -1, -1}, {1, 1, 1}, 1.f); kids[3].transform.col[3] = {0, 10, 0};
    kids[4].bound = prim(BoundType::Capsule, {-0.5f, -0.5f, -1.5f}, {0.5f, 0.5f, 1.5f}, 0.5f);
    kids[5].bound = prim(BoundType::Cylinder, {-0.5f, -0.5f, -1.f}, {0.5f, 0.5f, 1.f}, 0.04f);
    kids[6].bound = prim(BoundType::Disc, {-1, -1, -0.1f}, {1, 1, 0.1f}, 0.04f);
    auto comp = Bound::make_composite(kids);
    comp->room_ped_density = (2 << 5) | 5;
    rsc::write_file((fs::path(dir) / "synth_composite.ybn").string(), write_ybn(*comp));
    rsc::write_file((fs::path(dir) / "synth_plain.ybn").string(), write_ybn(*plain));
    rsc::write_file((fs::path(dir) / "synth_prim.ybn").string(), write_ybn(*prim(BoundType::Box, {-1, -1, -1}, {1, 1, 1}, 0.04f)));

    // textures of every format ragekit can encode
    TextureDictionary txd;
    struct F { TextureFormat f; const char* n; uint32_t w, h; };
    for (F f : {F{TextureFormat::DXT1, "tex_dxt1", 64, 32}, F{TextureFormat::DXT5, "tex_dxt5", 32, 32}, F{TextureFormat::ATI1, "tex_ati1", 16, 16},
                F{TextureFormat::ATI2, "tex_ati2", 16, 16}, F{TextureFormat::A8R8G8B8, "tex_argb", 8, 8}, F{TextureFormat::A8, "tex_a8", 8, 8},
                F{TextureFormat::L8, "tex_l8", 8, 8}, F{TextureFormat::A8B8G8R8, "tex_abgr", 8, 8}}) {
        Texture tx = encode_texture(gradient(f.w, f.h, (int)f.f % 5), f.f, f.n);
        tx.usage_data = (uint32_t)TextureUsage::Diffuse | (0x1000000u << 5) | (0x4u << 5);
        tx.extra_flags = 1; tx.unk32 = 0x40;
        txd.add(tx);
    }
    rsc::write_file((fs::path(dir) / "synth_textures.ytd").string(), write_ytd(txd));

    // a drawable
    auto make = [&](const std::string& name, bool lights) {
        Drawable d;
        d.name = name;
        ShaderGroup sg;
        sg.textures = TextureDictionary{};
        sg.textures->add(encode_texture(gradient(16, 16, 1), TextureFormat::DXT1, name + "_d"));
        Texture n = encode_texture(gradient(16, 16, 2), TextureFormat::DXT5, name + "_n"); n.usage_data = (uint32_t)TextureUsage::Normal;
        sg.textures->add(n);
        Shader sh;
        sh.name_hash = joaat_lower("normal_spec"); sh.sps_hash = joaat_lower("normal_spec.sps"); sh.render_bucket = 1;
        sh.params = {ShaderParam::tex("DiffuseSampler", name + "_d"), ShaderParam::tex("BumpSampler", name + "_n"), ShaderParam::tex("SpecSampler", ""),
                     ShaderParam::vec("specularFactor", {20.5f, 0, 0, 0}), ShaderParam::vec("specularColorFactor", {1.f / 3.f, 1e-7f, 123456.789f, -0.0f}),
                     ShaderParam::vec("my_unknown_param", {1, 2, 3, 4})};
        ShaderParam arr; arr.name_hash = joaat_lower("globalAnimUV0"); arr.type = ParamType::Array; arr.values = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        sh.params.push_back(arr);
        Shader sh2;
        sh2.name_hash = joaat_lower("default"); sh2.sps_hash = joaat_lower("default.sps");
        sh2.params = {ShaderParam::tex("DiffuseSampler", name + "_d")};
        sg.shaders = {sh, sh2};
        d.shader_group = sg;
        for (int lod = 0; lod < 3; lod++) {
            Model m;
            m.render_mask = 0xFF; m.flags = (uint8_t)lod;
            for (int gi = 0; gi < 2; gi++) {
                Geometry g;
                g.layout = VertexLayout::standard({VertexSemantic::Position, VertexSemantic::Normal, VertexSemantic::Colour0, VertexSemantic::TexCoord0,
                                                   VertexSemantic::TexCoord1, VertexSemantic::Tangent});
                g.resize_vertices(4);
                for (uint32_t i = 0; i < 4; i++) {
                    g.set(i, VertexSemantic::Position, {(float)(i & 1) * 0.1f + (float)gi, (float)(i >> 1) * 1.3f, 0.123456789f * (float)lod, 0});
                    g.set(i, VertexSemantic::Normal, {0, 0, 1, 0});
                    g.set(i, VertexSemantic::Colour0, {(float)i / 4.f, 0.5f, 1.f, 1.f});
                    g.set(i, VertexSemantic::TexCoord0, {(float)(i & 1), (float)(i >> 1), 0, 0});
                    g.set(i, VertexSemantic::TexCoord1, {(float)(i & 1) * 0.5f, (float)(i >> 1) * 0.25f, 0, 0});
                    g.set(i, VertexSemantic::Tangent, {1, 0, 0, 1});
                }
                g.indices = {0, 1, 2, 2, 1, 3};
                g.shader_index = (uint16_t)gi;
                if (lod == 0 && gi == 1) g.bone_ids = {0, 1};
                m.geometries.push_back(std::move(g));
            }
            d.lods[lod].push_back(std::move(m));
        }
        d.lod_distance = {20, 60, 150, 500};
        d.compute_bounds();
        Skeleton sk;
        sk.unk1C = 1;
        Bone b0; b0.name = "root"; b0.tag = 0; b0.index = 0; b0.parent = -1; b0.next_sibling = -1; b0.flags = 0x1 | 0x10;
        Bone b1; b1.name = "child_01"; b1.tag = 4; b1.index = 1; b1.parent = 0; b1.next_sibling = -1; b1.translation = {0, 0.5f, 0.25f}; b1.rotation = {0.1f, 0.2f, 0.3f, 0.9f};
        sk.bones = {b0, b1};
        d.skeleton = sk;
        {
            Joints j; // JointRotationLimit_s (0xC0 bytes: BoneId @8, unknown @10, Min @0x5C, Max @0x68), JointTranslationLimit_s (0x40: BoneId @8, Min @0x20, Max @0x30)
            j.rotation_limits.assign(0xC0 * 2, 0);
            j.translation_limits.assign(0x40, 0);
            auto put = [](Bytes& b, size_t off, auto v) { std::memcpy(b.data() + off, &v, sizeof v); };
            for (int i = 0; i < 2; i++) {
                size_t o = (size_t)i * 0xC0;
                put(j.rotation_limits, o + 8, (uint16_t)(5 + i)); put(j.rotation_limits, o + 10, (uint16_t)7);
                float mn[3] = {-1.5f, -2.f, -3.f - (float)i}, mx[3] = {1.f, 2.f, 3.f};
                std::memcpy(j.rotation_limits.data() + o + 0x5C, mn, 12); std::memcpy(j.rotation_limits.data() + o + 0x68, mx, 12);
            }
            put(j.translation_limits, 8, (uint32_t)9);
            float tmn[3] = {-1, -2, -3}, tmx[3] = {4, 5, 6};
            std::memcpy(j.translation_limits.data() + 0x20, tmn, 12); std::memcpy(j.translation_limits.data() + 0x30, tmx, 12);
            d.joints = j;
        }
        if (lights) {
            Light l;
            l.position = {1, 2, 3}; l.colour[0] = 255; l.colour[1] = 128; l.colour[2] = 7; l.intensity = 12.5f; l.flags = 0x1234; l.type = LightType::Spot;
            l.falloff = 7.5f; l.falloff_exponent = 4; l.cone_inner = 5; l.cone_outer = 40; l.direction = {0, 0, -1}; l.tangent = {1, 0, 0}; l.extent = {1, 1, 1};
            l.volume_intensity = 0.2f; l.corona_size = 0.5f; l.time_flags = 0xFFFFFF; l.shadow_fade = 40; l.light_fade = 30; l.projected_texture_hash = joaat_lower("light_tex");
            d.lights.push_back(l);
            l.type = LightType::Point; l.position = {-1, 0.5f, 0};
            d.lights.push_back(l);
            l.type = LightType::Capsule; l.extent = {0.1f, 2, 0.1f};
            d.lights.push_back(l);
            d.bound = comp;
        }
        return d;
    };
    Drawable a = make("synth_prop", true);
    rsc::write_file((fs::path(dir) / "synth_prop.ydr").string(), write_ydr(a));
    DrawableDictionary dd;
    for (const char* nm : {"synth_a", "synth_b"}) { dd.hashes.push_back(joaat(nm)); dd.drawables.push_back(make(nm, false)); }
    dd.hashes.push_back(0xDEADBEEF); dd.drawables.push_back(make("not_the_key", false));
    rsc::write_file((fs::path(dir) / "synth_dict.ydd").string(), write_ydd(dd));
    std::printf("synth files written to %s\n", dir.c_str());
    return 0;
}

// ---------------------------------------------------------------- gen

struct Counts { int unknown_params = 0, identical = 0, semantic = 0, different = 0, failed = 0, dds_same = 0, dds_diff = 0, dds_missing = 0; };

static void write_file(const fs::path& p, const void* data, size_t n) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write((const char*)data, (std::streamsize)n);
}

static int cmd_gen(const std::string& list, const std::string& rkdir, const std::string& cwdir) {
    std::map<std::string, Counts> by;
    std::vector<std::string> xmlin;
    int shown = 0;
    for (auto& path : read_lines(list)) {
        std::string ext = ext_of(path), fname = fs::path(path).filename().string(), stem = fs::path(path).stem().string();
        Counts& c = by[ext];
        try {
            Bytes file = rsc::read_file(path);
            std::string xml, exact; // exact: unknown shader parameters as numbers, like CodeWalker (compared with it); xml: the importable default
            std::vector<std::pair<std::string, Bytes>> dds;
            XmlOptions def, ex;
            ex.unknown_param_as_number = true;
            if (ext == ".ydr") { auto d = read_ydr(file); xml = write_drawable_xml(d, def); exact = write_drawable_xml(d, ex); dds = dds_files(d); }
            else if (ext == ".ydd") { auto d = read_ydd(file); xml = write_drawable_dictionary_xml(d, def); exact = write_drawable_dictionary_xml(d, ex); dds = dds_files(d); }
            else if (ext == ".ytd") { auto t = read_ytd(file); xml = exact = write_texture_dictionary_xml(t); dds = dds_files(t); }
            else if (ext == ".ybn") { auto b = read_ybn(file); xml = exact = write_bounds_xml(*b); }
            else continue;
            fs::path xp = fs::path(rkdir) / (fname + ".xml");
            write_file(xp, xml.data(), xml.size());
            for (auto& [n, bytes] : dds) write_file(fs::path(rkdir) / stem / n, bytes.data(), bytes.size());
            xmlin.push_back(xp.string() + "|" + (fs::path(rkdir) / stem).string());

            std::string cw = read_text((fs::path(cwdir) / (fname + ".xml")).string());
            std::string why;
            Cmp r = compare_text(exact, cw, why);
            if (xml != exact) c.unknown_params++;
            if (r == Cmp::Identical) c.identical++;
            else if (r == Cmp::Semantic) { c.semantic++; if (shown < 30) { shown++; std::printf("semantic  %s: %s\n", fname.c_str(), why.c_str()); } }
            else { c.different++; if (shown < 60) { shown++; std::printf("DIFFERENT %s: %s\n", fname.c_str(), why.c_str()); } }
            for (auto& [n, bytes] : dds) {
                std::string cwd = read_text((fs::path(cwdir) / stem / n).string());
                if (cwd == "<missing>") c.dds_missing++;
                else if (cwd.size() == bytes.size() && std::memcmp(cwd.data(), bytes.data(), bytes.size()) == 0) c.dds_same++;
                else c.dds_diff++;
            }
        } catch (const std::exception& e) {
            c.failed++;
            std::printf("FAIL %s: %s\n", fname.c_str(), e.what());
        }
    }
    int bad = 0;
    for (auto& [ext, c] : by) {
        std::printf("%-5s identical %4d  semantic %4d  different %4d  failed %3d   dds same %5d  differ %4d  missing %3d\n", ext.c_str(), c.identical,
                    c.semantic, c.different, c.failed, c.dds_same, c.dds_diff, c.dds_missing);
        bad += c.different + c.failed;
    }
    std::ofstream f(fs::path(rkdir) / "xmlin.txt");
    for (auto& l : xmlin) f << l << "\n";
    return bad ? 1 : 0;
}

// ---------------------------------------------------------------- summary

static uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    auto b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static std::string hx(uint64_t v) { char b[32]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v); return b; }

// CodeWalker's importer rebuilds the material table from the polygons (BuildMaterials): identical entries merge, unused ones go
static std::vector<std::pair<uint32_t, uint32_t>> used_materials(const Bound::Geometry& g) {
    std::vector<std::pair<uint32_t, uint32_t>> v;
    for (size_t i = 0; i < g.polygons.size(); i++) {
        size_t m = i < g.polygon_materials.size() ? g.polygon_materials[i] : 0;
        if (m < g.materials.size()) v.push_back({g.materials[m].data1, g.materials[m].data2});
        else v.push_back({0, 0});
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}
static size_t distinct_materials(const Bound::Geometry& g) { return used_materials(g).size(); }

static void sum_bound(std::vector<std::string>& out, const Bound& b, const std::string& pre) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "%stype %s margin %g mat %u proc %u room %u poly %u", pre.c_str(), bound_type_name(b.type),
                  b.margin, b.material_index, b.procedural_id, b.room_id(), b.poly_flags);
    out.push_back(buf);
    // CodeWalker recomputes a geometry bound's box (and quantum) from the re-quantised vertices on import: compared loosely ("~")
    std::snprintf(buf, sizeof buf, "~%sbox %g %g %g .. %g %g %g", pre.c_str(), b.box_min.x, b.box_min.y, b.box_min.z, b.box_max.x, b.box_max.y, b.box_max.z);
    out.push_back(buf);
    if (b.geometry) {
        auto& g = *b.geometry;
        int kinds[5] = {};
        // CodeWalker's BVH builder reorders the polygons of a GeometryBVH on import: hash them as a set
        uint64_t ph = 0, mh = 1469598103934665603ull;
        for (size_t i = 0; i < g.polygons.size(); i++) {
            auto& p = g.polygons[i];
            kinds[p.kind]++;
            uint8_t m = i < g.polygon_materials.size() ? g.polygon_materials[i] : 0;
            // the material itself, not its index: CodeWalker rebuilds the table on import
            uint32_t md[2] = {m < g.materials.size() ? g.materials[m].data1 : 0, m < g.materials.size() ? g.materials[m].data2 : 0};
            uint64_t h = fnv(md, 8);
            uint8_t kd = (uint8_t)p.kind;
            h = fnv(&kd, 1, h);
            for (int k = 0; k < p.vertex_count(); k++) { uint32_t v = p.vertex(k); h = fnv(&v, 4, h); }
            if (p.kind == BoundPolygon::Triangle) for (int k = 0; k < 3; k++) { uint8_t fl = p.vertex_flag(k); h = fnv(&fl, 1, h); }
            if (p.kind != BoundPolygon::Triangle && p.kind != BoundPolygon::Box) { float r = p.radius(); h = fnv(&r, 4, h); }
            ph += h;
        }
        std::snprintf(buf, sizeof buf, "%s  vertices %zu polygons %zu (tri %d sph %d cap %d box %d cyl %d) materials %zu colours %zu vcolours %zu polyhash %s",
                      pre.c_str(), g.vertices.size(), g.polygons.size(), kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], distinct_materials(g),
                      g.material_colours.size(), g.vertex_colours.size(), hx(ph).c_str());
        out.push_back(buf);
        uint64_t h = 1469598103934665603ull;
        uint64_t sum = 0;
        auto dm = used_materials(g);
        for (auto& m : dm) { uint32_t d[2] = {m.first, m.second}; sum += fnv(d, 8); }
        for (auto c : g.material_colours) sum += fnv(&c, 4);
        (void)h;
        out.push_back(pre + "  material set " + hx(sum));
        out.push_back(pre + "  bvh " + (g.bvh ? "yes" : "none")); // node counts differ: CodeWalker builds its own tree
    }
    for (size_t i = 0; i < b.children.size(); i++) {
        auto& c = b.children[i];
        std::snprintf(buf, sizeof buf, "%s  child %zu flags %08X %08X", pre.c_str(), i, c.flags1, c.flags2);
        out.push_back(buf);
        auto m = c.transform.matrix();
        uint64_t h = fnv(m.m.data(), 64);
        out.push_back(pre + "  child " + std::to_string(i) + " transform " + hx(h));
        if (c.bound) sum_bound(out, *c.bound, pre + "    ");
        else out.push_back(pre + "    (null)");
    }
}

// A NaN cannot carry its payload through text ("NaN" -> the canonical NaN): float components are hashed with NaNs canonicalised.
// Damaged game files (byte-swapped vertex buffers) are full of them.
static uint64_t vertex_hash(const Geometry& g) {
    uint64_t h = 1469598103934665603ull;
    uint32_t stride = g.layout.stride();
    for (uint32_t v = 0; v < g.vertex_count && (size_t)(v + 1) * stride <= g.vertices.size(); v++) {
        const uint8_t* base = g.vertices.data() + (size_t)v * stride;
        uint32_t off = 0;
        for (int k = 0; k < 16; k++) {
            if (!((g.layout.flags >> k) & 1)) continue;
            auto t = g.layout.type_of((VertexSemantic)k);
            uint32_t n = vertex_component_size(t);
            bool floats = t == VertexComponentType::Float || t == VertexComponentType::Float2 || t == VertexComponentType::Float3 || t == VertexComponentType::Float4;
            for (uint32_t i = 0; i < n; i += floats ? 4 : n) {
                uint32_t len = floats ? 4 : n;
                uint32_t w = 0;
                if (floats) {
                    std::memcpy(&w, base + off + i, 4);
                    if ((w & 0x7F800000u) == 0x7F800000u && (w & 0x7FFFFFu)) w = 0x7FC00000u;
                    h = fnv(&w, 4, h);
                } else h = fnv(base + off + i, len, h);
            }
            off += n;
        }
    }
    return h;
}

static void sum_drawable(std::vector<std::string>& out, const Drawable& d, const std::string& pre, bool skip_name = false) {
    char buf[512];
    if (!skip_name) out.push_back(pre + "name " + d.name);
    std::snprintf(buf, sizeof buf, "%sbounds sphere %g %g %g r %g box %g %g %g .. %g %g %g lod %g %g %g %g flags %08X %08X %08X %08X", pre.c_str(),
                  d.bounding_center.x, d.bounding_center.y, d.bounding_center.z, d.bounding_radius, d.bbox_min.x, d.bbox_min.y, d.bbox_min.z,
                  d.bbox_max.x, d.bbox_max.y, d.bbox_max.z, d.lod_distance[0], d.lod_distance[1], d.lod_distance[2], d.lod_distance[3],
                  d.render_mask_flags[0] & 0xFF, d.render_mask_flags[1] & 0xFF, d.render_mask_flags[2] & 0xFF, d.render_mask_flags[3] & 0xFF);
    out.push_back(buf);
    if (d.shader_group) {
        auto& sg = *d.shader_group;
        out.push_back(pre + "shaders " + std::to_string(sg.shaders.size()));
        for (size_t i = 0; i < sg.shaders.size(); i++) {
            auto& s = sg.shaders[i];
            out.push_back(pre + " shader " + std::to_string(i) + " " + hash_name(s.name_hash) + " " + hash_name(s.sps_hash) + " bucket " + std::to_string(s.render_bucket));
            for (auto& p : s.params) {
                std::string l = pre + "  param " + hash_name(p.name_hash) + " type " + std::to_string((int)p.type);
                if (p.type == ParamType::Texture) l += " tex '" + p.texture + "'";
                for (auto& v : p.values) { char t[128]; std::snprintf(t, sizeof t, " (%g %g %g %g)", v.x, v.y, v.z, v.w); l += t; }
                out.push_back(l);
            }
        }
        if (sg.textures) {
            out.push_back(pre + "textures " + std::to_string(sg.textures->textures.size()));
            std::vector<std::string> tl; // CodeWalker sorts a dictionary by name hash on import: compare as a set
            for (auto& t : sg.textures->textures) {
                std::snprintf(buf, sizeof buf, "%s tex %s %ux%u levels %u fmt %u usage %08X extra %u unk32 %u bytes %zu hash %s", pre.c_str(), t.name.c_str(), t.width,
                              t.height, t.levels, (unsigned)t.format, t.usage_data, t.extra_flags, t.unk32, t.data.size(), hx(fnv(t.data.data(), t.data.size())).c_str());
                tl.push_back(buf);
            }
            std::sort(tl.begin(), tl.end());
            for (auto& l : tl) out.push_back(l);
        } else out.push_back(pre + "no textures");
    } else out.push_back(pre + "no shader group");
    for (int l = 0; l < 4; l++) {
        out.push_back(pre + "lod " + std::to_string(l) + " models " + std::to_string(d.lods[l].size()));
        for (size_t m = 0; m < d.lods[l].size(); m++) {
            auto& mo = d.lods[l][m];
            std::snprintf(buf, sizeof buf, "%s model %zu mask %u flags %u binding %08X geoms %zu", pre.c_str(), m, mo.render_mask, mo.flags, mo.skeleton_binding, mo.geometries.size());
            out.push_back(buf);
            for (size_t g = 0; g < mo.geometries.size(); g++) {
                auto& ge = mo.geometries[g];
                std::string bones;
                for (auto b : ge.bone_ids) bones += std::to_string(b) + ",";
                std::snprintf(buf, sizeof buf, "%s  geom %zu shader %u verts %u indices %zu layout %08X/%016llX stride %u vb %u vhash %s ihash %s bones [%s] box %g %g %g %g .. %g %g %g %g", pre.c_str(), g,
                              ge.shader_index, ge.vertex_count, ge.indices.size(), ge.layout.flags, (unsigned long long)ge.layout.types, ge.layout.stride(), ge.vb_flags,
                              hx(vertex_hash(ge)).c_str(), hx(fnv(ge.indices.data(), ge.indices.size() * 2)).c_str(), bones.c_str(),
                              ge.bounds.min.x, ge.bounds.min.y, ge.bounds.min.z, ge.bounds.min.w, ge.bounds.max.x, ge.bounds.max.y, ge.bounds.max.z, ge.bounds.max.w);
                out.push_back(buf);
            }
        }
    }
    out.push_back(pre + "extra models " + std::to_string(d.lods_extra.size()));
    if (d.skeleton) {
        out.push_back(pre + "skeleton bones " + std::to_string(d.skeleton->bones.size()));
        for (auto& b : d.skeleton->bones) {
            std::snprintf(buf, sizeof buf, "%s bone %s tag %u idx %d parent %d sib %d flags %u t %g %g %g r %g %g %g %g s %g %g %g", pre.c_str(), b.name.c_str(), b.tag, b.index, b.parent,
                          b.next_sibling, b.flags, b.translation.x, b.translation.y, b.translation.z, b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w, b.scale.x, b.scale.y, b.scale.z);
            out.push_back(buf);
        }
    } else out.push_back(pre + "no skeleton");
    if (d.joints) {
        // rotation limits: only the fields the XML carries (BoneId, UnknownA, Min, Max); CodeWalker fills the rest with its defaults on import
        uint64_t h = 1469598103934665603ull;
        for (size_t o = 0; o + 0xC0 <= d.joints->rotation_limits.size(); o += 0xC0) {
            h = fnv(d.joints->rotation_limits.data() + o + 8, 4, h);
            h = fnv(d.joints->rotation_limits.data() + o + 0x5C, 24, h);
        }
        out.push_back(pre + "joints rot " + hx(h) + " tr " + hx(fnv(d.joints->translation_limits.data(), d.joints->translation_limits.size())));
    }
    out.push_back(pre + "lights " + std::to_string(d.lights.size()));
    for (size_t i = 0; i < d.lights.size(); i++) {
        auto& l = d.lights[i];
        std::snprintf(buf, sizeof buf, "%s light %zu pos %g %g %g col %u %u %u int %g type %u flags %u falloff %g/%g dir %g %g %g tan %g %g %g cone %g %g ext %g %g %g proj %08X group %u time %u bone %u hash %u fade %u %u %u %u", pre.c_str(),
                      i, l.position.x, l.position.y, l.position.z, l.colour[0], l.colour[1], l.colour[2], l.intensity, (unsigned)l.type, l.flags, l.falloff, l.falloff_exponent,
                      l.direction.x, l.direction.y, l.direction.z, l.tangent.x, l.tangent.y, l.tangent.z, l.cone_inner, l.cone_outer, l.extent.x, l.extent.y, l.extent.z,
                      l.projected_texture_hash, l.group_id, l.time_flags, l.bone_id, l.light_hash, l.light_fade, l.shadow_fade, l.specular_fade, l.volumetric_fade);
        out.push_back(buf);
    }
    if (d.bound) { out.push_back(pre + "bound"); sum_bound(out, *d.bound, pre + "  "); } else out.push_back(pre + "no bound");
}

static std::vector<std::string> summarize(const std::string& path) {
    std::vector<std::string> out;
    Bytes file = rsc::read_file(path);
    std::string ext = ext_of(path);
    if (ext == ".ydr") sum_drawable(out, read_ydr(file), "");
    else if (ext == ".ydd") {
        auto dd = read_ydd(file);
        out.push_back("drawables " + std::to_string(dd.drawables.size()));
        for (size_t i = 0; i < dd.drawables.size(); i++) { out.push_back("item " + std::to_string(i) + " hash " + hash_name(dd.hashes[i])); sum_drawable(out, dd.drawables[i], "  ", true); } // CodeWalker names a ydd item hash_XXXXXXXX when its name does not hash to the key
    } else if (ext == ".ytd") {
        auto t = read_ytd(file);
        out.push_back("textures " + std::to_string(t.textures.size()));
        char buf[512];
        for (auto& x : t.textures) {
            std::snprintf(buf, sizeof buf, " tex %s %ux%u levels %u fmt %u usage %08X extra %u unk32 %u bytes %zu hash %s", x.name.c_str(), x.width, x.height, x.levels, (unsigned)x.format,
                          x.usage_data, x.extra_flags, x.unk32, x.data.size(), hx(fnv(x.data.data(), x.data.size())).c_str());
            out.push_back(buf);
        }
    } else if (ext == ".ybn") {
        auto b = read_ybn(file);
        if (b) sum_bound(out, *b, ""); else out.push_back("null");
    }
    return out;
}

// "~" lines hold floats CodeWalker recomputes on import: equal when every number is within 12% of the box size (or 1e-3)
static bool lines_equal(const std::vector<std::string>& a, const std::vector<std::string>& b, size_t& at, bool& boxdev) {
    if (a.size() != b.size()) { at = std::min(a.size(), b.size()); for (size_t i = 0; i < at; i++) if (a[i] != b[i] && a[i][0] != '~') { at = i; return false; } return false; }
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] == b[i]) continue;
        if (a[i][0] == '~' && b[i][0] == '~') {
            auto ta = tokens(a[i]), tb = tokens(b[i]);
            double size = 0;
            for (auto& t : ta) { double v; if (as_number(t, v)) size = std::max(size, std::fabs(v)); }
            bool ok = ta.size() == tb.size();
            for (size_t k = 0; ok && k < ta.size(); k++) {
                double x, y;
                if (as_number(ta[k], x) && as_number(tb[k], y)) ok = std::fabs(x - y) <= std::max(1e-3, 0.12 * size);
                else ok = ta[k] == tb[k];
            }
            if (ok) continue;
            boxdev = true; // only informational
            continue;
        }
        at = i;
        return false;
    }
    return true;
}

static int cmd_summary(const std::string& list, const std::string& impdir) {
    std::map<std::string, std::array<int, 4>> by; // equal, different, failed, equal but with a recomputed box more than 12% off
    int shown = 0;
    for (auto& path : read_lines(list)) {
        std::string ext = ext_of(path), fname = fs::path(path).filename().string();
        if (ext != ".ydr" && ext != ".ydd" && ext != ".ytd" && ext != ".ybn") continue;
        auto& c = by[ext];
        std::string imp = (fs::path(impdir) / fname).string();
        if (!fs::exists(imp)) { c[2]++; if (shown < 400) { shown++; std::printf("MISSING import %s\n", fname.c_str()); } continue; }
        try {
            auto a = summarize(path), b = summarize(imp);
            size_t i = 0;
            bool boxdev = false;
            if (lines_equal(a, b, i, boxdev)) { c[0]++; if (boxdev) c[3]++; continue; }
            c[1]++;
            if (shown < 400) {
                shown++;
                std::printf("DIFF %s (line %zu of %zu/%zu)\n   orig: %s\n   imp : %s\n", fname.c_str(), i, a.size(), b.size(), i < a.size() ? a[i].c_str() : "<end>",
                            i < b.size() ? b[i].c_str() : "<end>");
            }
        } catch (const std::exception& e) {
            c[2]++;
            if (shown < 400) { shown++; std::printf("FAIL %s: %s\n", fname.c_str(), e.what()); }
        }
    }
    int bad = 0;
    for (auto& [ext, c] : by) {
        std::printf("%-5s summaries equal %4d (%d with a bound box CodeWalker recomputed more than 12%% off)  different %4d  failed/missing %3d\n", ext.c_str(), c[0], c[3], c[1], c[2]);
        bad += c[1] + c[2];
    }
    return bad ? 1 : 0;
}

int main(int argc, char** argv) {
    try {
        std::string cmd = argc > 1 ? argv[1] : "";
        if (cmd == "selftest") return selftest();
        if (cmd == "synth" && argc == 3) return cmd_synth(argv[2]);
        if (cmd == "floats" && argc == 3) return cmd_floats(argv[2]);
        if (cmd == "gen" && argc == 5) return cmd_gen(argv[2], argv[3], argv[4]);
        if (cmd == "summary" && argc == 4) return cmd_summary(argv[2], argv[3]);
        std::printf("usage: rk_xml_test selftest | synth <dir> | gen <list> <rk out dir> <cw dir> | summary <list> <imported dir>\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
