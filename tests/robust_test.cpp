// robust_test: malformed and hostile files must throw rk::Error (or be refused) without reading past a buffer, without
// allocating what a made-up size asks for and without recursing off the stack; good files are unaffected (the round
// trips over the game's files check that). No game files needed.
//   - RSC7: a pointer + size that wraps, an array count past the segment, page flags of hundreds of MB over a few bytes
//   - names: "hash_" and "0x" only with hex digits after them
//   - XML: a bare '&' before a real reference, a numeric reference without digits, elements nested 100,000 deep
//   - RBF: elements nested a million deep
//   - bounds: a polygon naming a vertex past the end, a composite that contains itself
//   - navmesh: a polygon past the index list, an index naming no vertex
//   - meta: a structure entry outside its structure
//   - PSO: a schema index whose entries all name one 65,535-entry element
//   - DDS / PNG: a mip count past a byte, an image size whose buffer size wraps
//   - the game index cache: a row naming an archive past the table
// Usage: rk_robust_test [group...] (rsc names xml rbf bounds navmesh meta pso image cache). Exit 0 = every check passed.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include "ragekit/bounds.h"
#include "ragekit/gameindex.h"
#include "ragekit/hash.h"
#include "ragekit/image.h"
#include "ragekit/meta.h"
#include "ragekit/navmesh.h"
#include "ragekit/pso.h"
#include "ragekit/rbf.h"
#include "ragekit/rpf.h"
#include "ragekit/rsc.h"
#include "ragekit/xml_dom.h"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

using namespace rk;
namespace fs = std::filesystem;

static int fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%-100s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

// True when f throws rk::Error (anything else, or nothing, is a failure; the message says which).
static bool throws_error(const std::function<void()>& f, std::string* message = nullptr) {
    try { f(); }
    catch (const Error& e) { if (message) *message = e.what(); return true; }
    catch (const std::exception& e) { std::printf("  threw %s instead of rk::Error\n", e.what()); return false; }
    std::printf("  did not throw\n");
    return false;
}

// True when f runs without throwing.
static bool runs(const std::function<void()>& f) {
    try { f(); return true; }
    catch (const std::exception& e) { std::printf("  threw %s\n", e.what()); return false; }
}

static uint64_t peak_memory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS c{};
    c.cb = sizeof c;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof c)) return c.PeakWorkingSetSize;
#endif
    return 0;
}

static void put32(Bytes& b, size_t at, uint32_t v) { std::memcpy(&b[at], &v, 4); }
static void put_be32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back((uint8_t)(v >> s)); }

static void rsc_checks() {
    // a small resource of one 8 KB system page
    Builder b;
    BlockId root = b.alloc(Segment::System, 64);
    b.put(root, 0, (uint32_t)0x12345678);
    const Bytes file = b.build(rsc::kYbn);
    const rsc::Resource res = rsc::Resource::load(file);
    check(throws_error([&] { res.at(rsc::kSystemBase + 16, SIZE_MAX - 8); }), "rsc: a pointer + size that wraps past 2^64 is refused");
    check(throws_error([&] { res.at(rsc::kSystemBase + 0x100000000ull, 4); }), "rsc: a pointer with its upper 32 bits set is in neither segment");
    Reader r(res);
    check(throws_error([&] { r.array<uint64_t>(rsc::kSystemBase, (size_t)1 << 40); }), "rsc: an array count past the segment throws before allocating");
    check(throws_error([&] { r.array<uint32_t>(rsc::kSystemBase + 8, res.system.size() / 4); }), "rsc: an array one item past the segment end throws");
    check(r.array<uint32_t>(rsc::kSystemBase, res.system.size() / 4).size() == res.system.size() / 4, "rsc: an array filling the segment exactly reads");

    // page flags claiming two segments of 255 MB over a deflate stream of a few bytes
    Bytes bomb = file;
    bomb.resize(16);
    const Bytes tiny = rsc::deflate(Bytes(64, 0));
    bomb.insert(bomb.end(), tiny.begin(), tiny.end());
    const rsc::PageFlags big = rsc::PageFlags::make(10, {0, 0, 0, 0, 0, 0, 1, 0, 0});   // one page of 0x200 << 10 << 2 = 2 MB
    rsc::PageFlags huge = rsc::PageFlags::make(13, {0, 0, 0, 0, 0, 0, 0, 0, 0});
    {
        std::array<uint32_t, 9> c{};
        c[0] = 1; c[1] = 1; c[2] = 1;   // 0x200 << 13 << 8 + << 7 + << 6 = 1 GB + 512 MB + 256 MB: past a segment
        try { huge = rsc::PageFlags::make(13, c); } catch (const Error&) {}
        c = {};
        c[1] = 1; c[2] = 1;              // 0x200 << 11 << 7 + << 6 = 128 + 64 MB: under a segment's limit
        huge = rsc::PageFlags::make(11, c);
    }
    (void)big;
    put32(bomb, 8, huge.value | (bomb[11] & 0xF0u) << 24);
    put32(bomb, 12, huge.value);
    const uint64_t before = peak_memory();
    std::string msg;
    const bool threw = throws_error([&] { rsc::Resource::load(bomb); }, &msg);
    check(threw, "rsc: pages of " + std::to_string(huge.total_size() >> 20) + " MB + " + std::to_string(huge.total_size() >> 20) +
          " MB over a 64 byte stream throw (" + msg + ")");
    const uint64_t after = peak_memory();
    if (before && after) check(after - before < (64ull << 20), "rsc: ... and cost " + std::to_string((after - before) >> 20) + " MB, not what the flags ask for");

    // inflate: an expected size of 1 GB for a stream of 64 zero bytes
    const uint64_t b2 = peak_memory();
    const Bytes out = rsc::inflate(tiny, (size_t)1 << 30);
    const uint64_t a2 = peak_memory();
    check(out.size() == 64, "rsc: inflate gives what the stream holds (64 bytes)");
    if (b2 && a2) check(a2 - b2 < (64ull << 20), "rsc: inflate with a made-up expected size of 1 GB costs " + std::to_string((a2 - b2) >> 20) + " MB");
}

static void name_checks() {
    check(hash_or_name("hash_lamppost") == joaat_lower("hash_lamppost"), "names: hash_lamppost is a name, not a hash");
    check(hash_or_name("hash_0000BEEF") == 0xBEEF, "names: hash_0000BEEF is the hash 0xBEEF");
    check(hash_or_name("0xprop") == joaat_lower("0xprop"), "names: 0xprop is a name");
    check(hash_or_name("0x1F") == 0x1F, "names: 0x1F is the hash 0x1F");
}

static void xml_checks() {
    const XmlElement a = parse_xml("<a>AT&T &amp; co</a>");
    check(a.text == "AT&T & co", "xml: a bare '&' before a real reference keeps the '&' and decodes the reference ('" + a.text + "')");
    const XmlElement b = parse_xml("<a>x&#xZZ;y&#;z&#65;</a>");
    check(b.text == "x&#xZZ;y&#;zA", "xml: numeric references without digits are kept as written ('" + b.text + "')");
    std::string deep;
    for (int i = 0; i < 100000; i++) deep += "<a>";
    for (int i = 0; i < 100000; i++) deep += "</a>";
    check(throws_error([&] { parse_xml(deep); }), "xml: elements nested 100,000 deep throw instead of running off the stack");
    std::string ok;
    for (int i = 0; i < 100; i++) ok += "<a>";
    for (int i = 0; i < 100; i++) ok += "</a>";
    check(runs([&] { parse_xml(ok); }), "xml: 100 levels parse");
}

static void rbf_checks() {
    // a million elements, each the only child of the one before: the reading is a loop, the tree's destruction recursed
    Bytes f = {'R', 'B', 'F', '0'};
    const int depth = 1000000;
    for (int i = 0; i < depth; i++) {
        f.push_back(0);
        f.push_back(0);
        if (i == 0) { f.push_back(1); f.push_back(0); f.push_back('a'); }   // the name, declared at its first use
        for (int k = 0; k < 6; k++) f.push_back(0);                       // u32 0, no attributes
    }
    for (int i = 0; i < depth; i++) { f.push_back(0xFF); f.push_back(0xFF); }
    check(throws_error([&] { read_rbf(f); }), "rbf: elements nested a million deep throw");
    Bytes ok = {'R', 'B', 'F', '0', 0, 0, 1, 0, 'a', 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    check(runs([&] { read_rbf(ok); }), "rbf: one element reads");
}

static void bound_checks() {
    Bound g;
    g.type = BoundType::Geometry;
    g.geometry.emplace();
    g.geometry->vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    BoundPolygon p = BoundPolygon::triangle(0, 1, 5);
    g.geometry->polygons.push_back(p);
    g.geometry->materials.push_back({});
    check(throws_error([&] { g.finalize(); }), "bounds: finalize refuses a polygon naming vertex 5 of 3");

    // a composite whose child list names the composite itself
    Bound c;
    c.type = BoundType::Composite;
    BoundChild ch;
    ch.bound = std::make_shared<Bound>();
    ch.bound->type = BoundType::Sphere;
    ch.bound->sphere_radius = 1;
    c.children.push_back(ch);
    Bytes file = write_ybn(c);
    rsc::Resource res = rsc::Resource::load(file);
    Reader r(res);
    const uint64_t list = r.u64(rsc::kSystemBase + 0x70);
    const uint64_t self = rsc::kSystemBase;
    std::memcpy(&res.system[(size_t)(list - rsc::kSystemBase)], &self, 8);
    const Bytes bad = res.save();
    check(throws_error([&] { read_ybn(bad); }), "bounds: a composite that contains itself throws");
}

static void navmesh_checks() {
    NavMesh n;
    n.size = {10, 10, 10};
    n.vertices = {{0, 0, 0}, {1000, 0, 0}, {0, 1000, 0}};
    n.indices = {0, 1, 2};
    n.edges.resize(3);
    NavPoly p;
    p.set_vertex_count(3);
    n.polys = {p};
    check(runs([&] { read_ynv(write_ynv(n)); }), "navmesh: a one-triangle cell reads");
    NavMesh past = n;
    past.polys[0].first_index = 2;
    check(throws_error([&] { read_ynv(write_ynv(past)); }), "navmesh: a polygon whose corners run past the index list throws");
    NavMesh novertex = n;
    novertex.indices[2] = 7;
    check(throws_error([&] { read_ynv(write_ynv(novertex)); }), "navmesh: an index naming vertex 7 of 3 throws");
}

static void meta_checks() {
    Meta m;
    MetaStruct s;
    s.name = 0x1234567;
    s.key = 1;
    s.size = 16;
    MetaEntry e;
    e.name = 0x2345678;
    e.offset = 1000;   // far outside the 16 byte structure
    e.type = metatype::CharPointer;
    s.entries.push_back(e);
    m.structs.push_back(s);
    MetaBlock blk;
    blk.type = s.name;
    blk.data.assign(16, 0);
    m.blocks.push_back(blk);
    const Meta back = read_meta(write_meta(m));
    check(throws_error([&] { detach_object(back, make_meta_pointer(1, 0)); }), "meta: a structure entry at offset 1000 of a 16 byte structure throws");
}

static void pso_checks() {
    // PSIN with nothing in it, then a PSCH whose 100,000 index entries all name one element of 65,535 entries
    Bytes f;
    put_be32(f, kPsoPSIN);
    put_be32(f, 16);
    for (int i = 0; i < 8; i++) f.push_back('p');
    Bytes sch;
    const uint32_t count = 100000;
    put_be32(sch, count);
    const uint32_t element = 8 + 4 + count * 8;   // element offsets count from the section's start, header included
    for (uint32_t i = 0; i < count; i++) { put_be32(sch, 0xABCD0000 + i); put_be32(sch, element); }
    put_be32(sch, 0xFFFF);   // kind 0 (structure), 65,535 entries
    put_be32(sch, 64);
    put_be32(sch, 0);
    sch.resize(sch.size() + 65535 * 12, 0);
    put_be32(f, kPsoPSCH);
    put_be32(f, (uint32_t)(8 + sch.size()));
    f.insert(f.end(), sch.begin(), sch.end());
    const auto t0 = std::chrono::steady_clock::now();
    bool schema = true;
    const bool read = runs([&] { Pso p = read_pso(f); schema = p.has_schema; });
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(read && !schema && s < 5, "pso: a schema naming one 65,535-entry element 100,000 times is refused in " + std::to_string(s).substr(0, 5) + " s");
}

static void image_checks() {
    // DDS: a 4x4 DXT1 with 300 mips claimed (a byte holds 255)
    Texture t = encode_texture(Image{4, 4, std::vector<uint8_t>(64, 128)}, TextureFormat::DXT1, "a", 1);
    Bytes dds = write_dds(t);
    put32(dds, 4 + 4, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);
    put32(dds, 4 + 24, 300);
    dds.resize(dds.size() + 300 * 8, 0);
    check(throws_error([&] { read_dds(dds, "a"); }), "dds: 300 mip levels throw (the level count is a byte)");
    Bytes wide = write_dds(t);
    put32(wide, 4 + 12, 70000);
    check(throws_error([&] { read_dds(wide, "a"); }), "dds: a width of 70000 throws (a texture's width is 16 bits)");

    // PNG: 0x40000000 x 0x40000001 RGBA, whose (stride + 1) * height wraps size_t on no platform but asks for exabytes
    Image img{2, 2, std::vector<uint8_t>(16, 200)};
    Bytes png = write_png(img);
    // IHDR data starts at 16: width, height
    png[16] = 0x40; png[17] = 0; png[18] = 0; png[19] = 0;
    png[20] = 0x40; png[21] = 0; png[22] = 0; png[23] = 1;
    check(throws_error([&] { read_png(png); }), "png: a 2^30 x 2^30 image over a few bytes of data throws before allocating");
    // the signature, a 5 byte chunk, then an empty IHDR as the file's last 8 bytes: its 13 bytes of fields lie past the end
    Bytes cut(png.begin(), png.begin() + 8);
    put_be32(cut, 5);
    for (char c : std::string("abcd")) cut.push_back((uint8_t)c);
    cut.resize(cut.size() + 5 + 4, 0);
    put_be32(cut, 0);
    for (char c : std::string("IHDR")) cut.push_back((uint8_t)c);
    std::string why;
    const bool threw = throws_error([&] { read_png(cut); }, &why);
    check(threw && why.find("IHDR") != std::string::npos, "png: an IHDR whose 13 bytes lie past the end of the file throws for that (" + why + ")");
}

static void cache_checks() {
    // a fake install: GTA5.exe and one archive holding one file, indexed into a cache of its own
    const fs::path dir = fs::temp_directory_path() / "rk_robust_game";
    const fs::path cache = fs::temp_directory_path() / "rk_robust_cache";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(cache, ec);
    fs::create_directories(dir);
    { std::ofstream(dir / "GTA5.exe") << "x"; }
    RpfBuilder rb;
    rb.add_binary("robust_file.dat", Bytes(100, 7), false);
    rb.save((dir / "x64a.rpf").string());
    GameIndexOptions o;
    o.cache_folder = cache.string();
    o.threads = 1;
    GameIndex first;
    try { first = GameIndex::open(dir.string(), o); }
    catch (const Error& e) { std::printf("  game index of the fake install: %s (skipped)\n", e.what()); return; }
    check(first.files().size() == 1, "cache: the fake install indexes its one file");
    // the IndexedFile row: name hash, then archive 0
    Bytes bin = rsc::read_file(first.info().cache_path);
    const uint32_t h = joaat_lower("robust_file");
    size_t at = SIZE_MAX;
    for (size_t i = 0; i + 8 <= bin.size(); i++) {
        uint32_t a, b;
        std::memcpy(&a, &bin[i], 4);
        std::memcpy(&b, &bin[i + 4], 4);
        if (a == h && b == 0) { at = i; break; }
    }
    check(at != SIZE_MAX, "cache: the file's row is found");
    if (at == SIZE_MAX) return;
    put32(bin, at + 4, 7);   // archive 7 of 1
    rsc::write_file(first.info().cache_path, bin);
    GameIndex second = GameIndex::open(dir.string(), o);
    check(!second.info().from_cache, "cache: a row naming archive 7 of 1 makes the cache unusable (the install is scanned again)");
    check(second.files().size() == 1 && second.archive_path(second.files()[0]).find("x64a.rpf") != std::string::npos, "cache: ... and the new index is whole");
    fs::remove_all(dir, ec);
    fs::remove_all(cache, ec);
}

int main(int argc, char** argv) {
    // names on the command line run only those groups (a crash in one, before a fix, then does not hide the others)
    const std::pair<const char*, void (*)()> groups[] = {{"rsc", rsc_checks}, {"names", name_checks}, {"xml", xml_checks}, {"rbf", rbf_checks}, {"bounds", bound_checks},
        {"navmesh", navmesh_checks}, {"meta", meta_checks}, {"pso", pso_checks}, {"image", image_checks}, {"cache", cache_checks}};
    for (auto& [name, run] : groups) {
        bool wanted = argc < 2;
        for (int i = 1; i < argc; i++) wanted |= std::strcmp(argv[i], name) == 0;
        if (wanted) run();
    }
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
