// rk_shaderdb_test [cache]: the ShaderDb rules without the game (a hand-built database), and with a cache written by
// `rk shaders build` the layouts of the shaders interiors use, as measured on the game's drawables.
#include <cmath>
#include <cstdio>
#include <string>
#include "ragekit/ragekit.h"

using namespace rk;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_fail++;
}

// the parameter list as "name@slot ..." (names from the base definition)
static std::string layout(const ShaderDb& db, const ShaderPresetDef& p) {
    const ShaderBaseDef* b = db.base(p.base);
    std::string out;
    for (const ShaderParam& prm : db.instantiate(p).params) {
        std::string n = "?";
        if (b) for (auto& d : b->textures) if (joaat_lower(d.name) == prm.name_hash) n = d.name;
        if (b) for (auto& d : b->vectors) if (joaat_lower(d.name) == prm.name_hash) n = d.name;
        out += (out.empty() ? "" : " ") + n + "@" + std::to_string(prm.slot);
    }
    return out;
}

int main(int argc, char** argv) {
    {   // .sps text
        SpsPreset s = read_sps("shader normal_spec \r\n__rage_drawbucket {\r\n\tint 3\r\n}\t", "normal_spec_cutout");
        check(s.base == "normal_spec" && s.bucket == 3, "read_sps: base normal_spec, bucket 3");
        SpsPreset g = read_sps("shader emissive_clip", "emissive_clip");
        check(g.base == "emissive_clip" && g.bucket == 0, "read_sps: no draw bucket = 0");
    }
    {   // instantiate on a hand-built base: textures in order with their slots, vectors reversed from 160 up
        ShaderDb db;
        ShaderBaseDef b;
        b.name = "test";
        ShaderParamDef t; t.texture = true; t.name = "DiffuseSampler"; t.slot = 0; b.textures.push_back(t);
        t.name = "BumpSampler"; t.slot = 2; b.textures.push_back(t);
        ShaderParamDef v; v.name = "first"; v.value = {1, 0, 0, 0}; b.vectors.push_back(v);
        v.name = "second"; v.value = {2, 0, 0, 0}; b.vectors.push_back(v);
        v.name = "third"; v.value = {3, 0, 0, 0}; v.count = 2; b.vectors.push_back(v);
        db.add_base(b);
        db.add_preset({"test", "test", 0});
        db.add_preset({"test_cutout", "test", 3});
        Shader s = db.instantiate(*db.preset("test_cutout"), {{"BumpSampler", "my_n"}});
        check(s.name_hash == joaat_lower("test") && s.sps_hash == joaat_lower("test_cutout.sps") && s.render_bucket == 3, "instantiate: Name = base, sps = preset, bucket");
        check(layout(db, *db.preset("test")) == "DiffuseSampler@0 BumpSampler@2 third@162 second@161 first@160", "instantiate: order and slots");
        check(s.params.size() == 5 && s.params[1].texture == "my_n" && s.params[0].texture.empty(), "instantiate: texture binding");
        check(s.params[2].type == ParamType::Array && s.params[2].values.size() == 2, "instantiate: arrays");
        check(db.preset_for("test", 3) && db.preset_for("test", 3)->name == "test_cutout" && db.preset_for("test", 0)->name == "test", "preset_for");
        const std::string tmp = "rk_shaderdb_test.db";
        check(db.save(tmp), "save");
        ShaderDb back = ShaderDb::load(tmp);
        check(back.base_count() == 1 && back.preset_count() == 2 && layout(back, *back.preset("test_cutout")) == layout(db, *db.preset("test_cutout")), "save / load round trip");
        std::remove(tmp.c_str());
    }
    if (argc > 1) {   // the game's database: the layouts measured on its drawables (tools/scan_shaders in the editor repo)
        ShaderDb db = ShaderDb::load(argv[1]);
        check(db.base_count() > 300 && db.preset_count() > 300, "game database loaded");
        struct Want { const char* preset; const char* layout; int bucket; };
        const Want wants[] = {
            {"default", "DiffuseSampler@0 matMaterialColorScale@165 HardAlphaBlend@164 useTessellation@163 wetnessMultiplier@162 globalAnimUV1@161 globalAnimUV0@160", 0},
            {"cutout", "DiffuseSampler@0 matMaterialColorScale@165 HardAlphaBlend@164 useTessellation@163 wetnessMultiplier@162 globalAnimUV1@161 globalAnimUV0@160", 3},
            {"normal_spec", "DiffuseSampler@0 BumpSampler@2 SpecSampler@3 HardAlphaBlend@167 useTessellation@166 wetnessMultiplier@165 bumpiness@164 specMapIntMask@163 specularIntensityMult@162 specularFalloffMult@161 specularFresnel@160", 0},
            {"normal", "DiffuseSampler@0 BumpSampler@2 HardAlphaBlend@168 useTessellation@167 wetnessMultiplier@166 bumpiness@165 specularIntensityMult@164 specularFalloffMult@163 specularFresnel@162 globalAnimUV1@161 globalAnimUV0@160", 0},
            {"spec", "DiffuseSampler@0 SpecSampler@2 HardAlphaBlend@168 useTessellation@167 wetnessMultiplier@166 specMapIntMask@165 specularIntensityMult@164 specularFalloffMult@163 specularFresnel@162 globalAnimUV1@161 globalAnimUV0@160", 0},
            {"glass_env", "DiffuseSampler@0 BumpSampler@2 SpecSampler@3 useTessellation@167 bumpiness@166 specMapIntMask@165 specularIntensityMult@164 specularFalloffMult@163 specularFresnel@162 globalAnimUV1@161 globalAnimUV0@160", 1},
            {"emissive", "DiffuseSampler@0 matMaterialColorScale@165 HardAlphaBlend@164 useTessellation@163 emissiveMultiplier@162 globalAnimUV1@161 globalAnimUV0@160", 0},
            {"emissive_clip", "DiffuseSampler@0 matMaterialColorScale@165 HardAlphaBlend@164 useTessellation@163 emissiveMultiplier@162 globalAnimUV1@161 globalAnimUV0@160", 0},
            {"normal_spec_cutout", "DiffuseSampler@0 BumpSampler@2 SpecSampler@3 HardAlphaBlend@167 useTessellation@166 wetnessMultiplier@165 bumpiness@164 specMapIntMask@163 specularIntensityMult@162 specularFalloffMult@161 specularFresnel@160", 3},
            // decals (draw bucket 2): the install's decal.fxc has HardAlphaBlend (the first variable of the buffer, slot 165); the drawables of the game that were compiled
            // earlier (tools/scan_decals in the editor repo: 2,756 decal geometries in interiors) carry the same list from useTessellation (164) down without it
            {"decal", "DiffuseSampler@0 HardAlphaBlend@165 useTessellation@164 wetnessMultiplier@163 specularIntensityMult@162 specularFalloffMult@161 specularFresnel@160", 2},
            {"decal_dirt", "DiffuseSampler@0 HardAlphaBlend@165 useTessellation@164 specularIntensityMult@163 specularFalloffMult@162 specularFresnel@161 dirtDecalMask@160", 2},
        };
        for (const Want& w : wants) {
            const ShaderPresetDef* p = db.preset(w.preset);
            check(p && p->bucket == w.bucket && layout(db, *p) == w.layout, std::string("game layout of ") + w.preset);
        }
    }
    std::printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
