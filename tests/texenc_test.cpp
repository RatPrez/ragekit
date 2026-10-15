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
    check("warm noise", noise, TextureFormat::DXT1, 14.5);
    check("alpha ramp", alpha, TextureFormat::DXT5, 2.0, 2.0);
    check("alpha cutout", cut, TextureFormat::DXT5, 2.0, 1.0);
    check("alpha cutout dxt3", cut, TextureFormat::DXT3, 2.0, 1.0);
    std::printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
