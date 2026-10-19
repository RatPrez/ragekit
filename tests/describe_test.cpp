// rk_describe_test: describe() on files built in memory: the facts it gives and the text format_facts makes of them.
// (rk_roundtrip --survey runs describe on every file it samples of an install.)
#include <cstdio>
#include <string>
#include "ragekit/ragekit.h"

using namespace rk;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

static const Fact* child(const Fact& f, const std::string& label) {
    for (const Fact& c : f.children) if (c.label == label) return &c;
    return nullptr;
}

int main() {
    // a texture dictionary of 30 textures: the list stops at max_items without verbose
    TextureDictionary txd;
    Image img;
    img.width = img.height = 4;
    img.rgba.assign(64, 200);
    for (int i = 0; i < 30; i++) txd.add(encode_texture(img, TextureFormat::DXT1, "tex" + std::to_string(i)));
    const Bytes ytd = write_ytd(txd);
    const Fact f = describe(ytd, "x.ytd");
    check(f.label == "texture dictionary" && f.value == "30 textures", "a ytd's root (" + f.label + ": " + f.value + ")");
    const Fact* t = child(f, "textures");
    check(t && t->value == "30" && t->children.size() == 21 && t->children.back().label == "...", "the list stops after 20 and says how many more");
    check(child(f, "RSC7") && child(*child(f, "RSC7"), "graphics"), "the resource header");
    DescribeOptions all;
    all.verbose = true;
    const Fact v = describe(ytd, "x.ytd", all);
    check(child(v, "textures")->children.size() == 30, "verbose lists every texture");
    const std::string text = format_facts(v);
    check(text.rfind("texture dictionary: 30 textures\n", 0) == 0 && text.find("\n    tex0: 4x4 D3DFMT_DXT1") != std::string::npos, "format_facts indents by level");

    // a text table, a file of no known kind, and a malformed one
    TextTable tt;
    tt.set(joaat_lower("A"), "first");
    const Fact g = describe(write_gxt2(tt), "y.gxt2");
    check(g.label == "text table" && g.children.size() == 1 && g.children[0].value == "first", "a gxt2");
    const Bytes junk = {1, 2, 3, 4, 5};
    check(describe(junk, "z.bin").label == "file", "a file of no known kind");
    bool threw = false;
    Bytes cut(ytd.begin(), ytd.begin() + 40);
    try { (void)describe(cut, "x.ytd"); } catch (const Error&) { threw = true; }
    check(threw, "a truncated resource throws");
    // JSON: escapes, a surrogate pair, numbers as written, key order, a round trip; errors with their offset
    const JsonValue j = parse_json("\xEF\xBB\xBF{\"b\":[1,-2.50,1e3,true,null],\"a\":\"x\\\"\\u00e9\\ud83d\\ude00\\n\"}");
    check(j.members.size() == 2 && j.members[0].first == "b" && j.members[1].first == "a", "json: members in the file's order");
    check(j.get("b")->items[1].text == "-2.50" && j.get("b")->items[2].number() == 1000 && j.get("b")->items[3].boolean, "json: numbers keep their text");
    check(j.get("a")->text == "x\"\xC3\xA9\xF0\x9F\x98\x80\n", "json: escapes and a surrogate pair as UTF-8");
    check(parse_json(write_json(j)) == j && parse_json(write_json(j, false)) == j, "json: written and read back, indented and compact");
    threw = false;
    try { (void)parse_json("{\"a\":[1,2}"); } catch (const Error& e) { threw = std::string(e.what()).find("byte 9") != std::string::npos; }
    check(threw, "json: a syntax error says where");
    // a heightmap: rows stored from their first to their last cell, the lowest heights after the highest
    Heightmap hm;
    hm.width = 4;
    hm.height = 2;
    hm.min = {-100, -50, 0};
    hm.max = {100, 50, 255};
    hm.max_heights = {0, 10, 20, 0, 0, 0, 0, 0};
    hm.min_heights = {0, 5, 6, 0, 0, 0, 0, 0};
    const Bytes hb = write_heightmap(hm);
    check(hb.size() == 44 + 16 + 4 && hb[0] == 'H' && hb[44 + 1] == 1 && hb[44 + 3] == 2 && hb[44 + 7] == 0xFF, "heightmap: big endian, row 0 from cell 1 (offset -1)");
    const Heightmap hr = read_heightmap(hb);
    check(hr.max_heights == hm.max_heights && hr.min_heights == hm.min_heights && write_heightmap(hr) == hb, "heightmap: read back the same");
    float lo = 0, hi = 0;
    check(hr.heights_at(-40, -30, lo, hi) && lo == 5 && hi == 10 && !hr.heights_at(60, 30, lo, hi) && !hr.heights_at(150, 0, lo, hi), "heightmap: heights at a position");
    check(describe(hb, "heightmap.dat").label == "heightmap", "heightmap: described");
    threw = false;
    try { (void)read_heightmap(Bytes(hb.begin(), hb.end() - 1)); } catch (const Error&) { threw = true; }
    check(threw, "heightmap: a truncated file throws");
    std::printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
