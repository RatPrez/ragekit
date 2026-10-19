// texenc_test: block-compression quality. Encodes synthetic images whose 4x4 blocks hold colours along non-axis lines (where a
// bounding-box encoder picks the wrong diagonal), gradients, noise and alpha ramps, decodes them, and checks the RMS error
// against fixed limits. Prints the errors so a change of encoder can be compared. Exit 0 = every limit met.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "ragekit/image.h"
#include "ragekit/texture.h"

using namespace rk;

static Image make(uint32_t w, uint32_t h, auto fn) {
    Image img;
    img.width = w; img.height = h;
    img.rgba.resize((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) fn(x, y, img.rgba.data() + ((size_t)y * w + x) * 4);
    return img;
}

static double rms(const Image& a, const Image& b, int c0, int c1) {
    double s = 0; size_t n = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        for (int c = c0; c <= c1; c++) { double d = (double)a.rgba[i + c] - b.rgba[i + c]; s += d * d; n++; }
    return std::sqrt(s / (double)n);
}

static int fails = 0;
static void check(const char* name, const Image& img, TextureFormat f, double limit_rgb, double limit_a = 1e9) {
    Texture t = encode_texture(img, f, name, 1);
    Image d = decode_texture(t, 0);
    const double e = rms(img, d, 0, 2), ea = rms(img, d, 3, 3);
    const bool ok = e <= limit_rgb && ea <= limit_a;
    std::printf("%-28s %-5s rgb rms %6.2f (<= %5.2f)  alpha rms %6.2f  %s\n", name, texture_format_name(f), e, limit_rgb, ea, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main() {
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) & 0xFF; };
    // every block: a ramp from red to green (a bounding box picks yellow and black as its ends)
    Image diag = make(64, 64, [](uint32_t x, uint32_t, uint8_t* p) { int t = (int)(x % 4) * 85; p[0] = (uint8_t)(255 - t); p[1] = (uint8_t)t; p[2] = 0; p[3] = 255; });
    // blue to orange along the anti-diagonal of the RGB cube
    Image anti = make(64, 64, [](uint32_t x, uint32_t y, uint8_t* p) { int t = (int)((x + y) % 4) * 85; p[0] = (uint8_t)t; p[1] = (uint8_t)(t / 2); p[2] = (uint8_t)(255 - t); p[3] = 255; });
    Image grad = make(128, 128, [](uint32_t x, uint32_t y, uint8_t* p) { p[0] = (uint8_t)(x * 2); p[1] = (uint8_t)(y * 2); p[2] = (uint8_t)(255 - x); p[3] = 255; });
    Image noise = make(64, 64, [&](uint32_t, uint32_t, uint8_t* p) { int g = (int)rnd(); p[0] = (uint8_t)std::min(255, g + 20); p[1] = (uint8_t)g; p[2] = (uint8_t)(g / 2); p[3] = 255; });
    Image alpha = make(64, 64, [](uint32_t x, uint32_t y, uint8_t* p) { p[0] = 200; p[1] = 180; p[2] = 160; p[3] = (uint8_t)((x * 4 + y) & 0xFF); });
    Image cut = make(64, 64, [](uint32_t x, uint32_t y, uint8_t* p) { p[0] = 90; p[1] = 120; p[2] = 60; p[3] = ((x / 3 + y / 5) % 2) ? 255 : 0; });
    // limits: what a principal-axis encoder with endpoint refinement reaches (with room); the bounding-box encoder fails diag / anti
    // (85) and the solid-colour alpha ramp (3.1); uniform noise along a line cannot go much below 14 with 4 levels a block
    check("red-green ramp", diag, TextureFormat::DXT1, 14.0);
    check("blue-orange ramp", anti, TextureFormat::DXT1, 14.0);
    check("gradient", grad, TextureFormat::DXT1, 3.0);
    // 14.05 before the encoder scored its candidates with the decoder's own (rounded) palette and refined them in 565 steps
    check("warm noise", noise, TextureFormat::DXT1, 14.0);

    // the decoders against D3D's rules: 565 widened by bit replication, palettes interpolated exactly and rounded
    {
        // c0 = red 4 (33), c1 = red 1 (8); pixels 0..3 take indices 0..3: 33, 8, round(74 / 3) = 25, round(49 / 3) = 16
        const uint8_t bc1[8] = {0x00, 0x20, 0x00, 0x08, 0xE4, 0, 0, 0};
        const Image d = decode_texels(TextureFormat::DXT1, 4, 4, bc1);
        const bool ok = d.px(0, 0)[0] == 33 && d.px(1, 0)[0] == 8 && d.px(2, 0)[0] == 25 && d.px(3, 0)[0] == 16;
        std::printf("%-28s %d %d %d %d (33 8 25 16)  %s\n", "bc1 palette", d.px(0, 0)[0], d.px(1, 0)[0], d.px(2, 0)[0], d.px(3, 0)[0], ok ? "ok" : "FAIL");
        if (!ok) fails++;
        // BC4 a0 = 200, a1 = 101, pixel 0 index 2: round((6 * 200 + 101) / 7) = 186
        const uint8_t bc4[8] = {200, 101, 2, 0, 0, 0, 0, 0};
        const Image e = decode_texels(TextureFormat::ATI1, 4, 4, bc4);
        const bool ok4 = e.px(0, 0)[0] == 186;
        std::printf("%-28s %d (186)  %s\n", "bc4 palette", e.px(0, 0)[0], ok4 ? "ok" : "FAIL");
        if (!ok4) fails++;
        // DXT3 alpha 9 is nearest to 17 (index 1), not 0
        Image a = make(4, 4, [](uint32_t, uint32_t, uint8_t* p) { p[0] = p[1] = p[2] = 0; p[3] = 9; });
        const Texture t = encode_texture(a, TextureFormat::DXT3, "a", 1);
        const bool ok3 = decode_texture(t).px(0, 0)[3] == 17;
        std::printf("%-28s %d (17)  %s\n", "dxt3 alpha 9", decode_texture(t).px(0, 0)[3], ok3 ? "ok" : "FAIL");
        if (!ok3) fails++;
    }
    check("alpha ramp", alpha, TextureFormat::DXT5, 2.0, 2.0);
    check("alpha cutout", cut, TextureFormat::DXT5, 2.0, 1.0);
    check("alpha cutout dxt3", cut, TextureFormat::DXT3, 2.0, 1.0);
    // BC7 (mode 6 only): better than DXT1 / DXT5 on everything but blocks of many unrelated colours
    check("red-green ramp bc7", diag, TextureFormat::BC7, 3.0);
    check("blue-orange ramp bc7", anti, TextureFormat::BC7, 3.0);
    check("gradient bc7", grad, TextureFormat::BC7, 1.5);
    check("warm noise bc7", noise, TextureFormat::BC7, 9.0);
    check("alpha ramp bc7", alpha, TextureFormat::BC7, 1.0, 2.0);
    check("alpha cutout bc7", cut, TextureFormat::BC7, 1.0, 1.0);
    std::printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
