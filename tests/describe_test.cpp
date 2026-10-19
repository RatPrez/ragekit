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
    std::printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
