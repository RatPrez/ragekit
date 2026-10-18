// bench: timings of the hot paths over the game install (skipped without one).
//   - GameIndex::open from its cache (what a tool's start-up costs), and with --scan a rebuild into a scratch cache folder
//   - the biggest .ytd: extract, read_ytd, decode of every level of every texture, write_ytd
//   - the biggest .ydr: read_ydr, write_ydr; the biggest .ybn: read_ybn, write_ybn (BVH built again)
//   - with --encode: encode_texture to DXT1 / DXT5 of the decoded level 0 of the biggest ytd's textures, and the PSNR of the result
//   - with --rpf: every archive of the index opened, and every entry checked to lie inside its archive
// Usage: rk_bench [--scan] [--rpf] [--encode] [--runs=N]. Each figure is the best of N runs (default 5).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include "ragekit/bounds.h"
#include "ragekit/drawable.h"
#include "ragekit/gameindex.h"
#include "ragekit/image.h"
#include "ragekit/rsc.h"
#include "ragekit/texture.h"

using namespace rk;

static int g_runs = 5;

static double best_of(const std::function<void()>& f) {
    double best = 1e30;
    for (int i = 0; i < g_runs; i++) {
        auto t0 = std::chrono::steady_clock::now();
        f();
        best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    return best;
}

static void line(const char* what, double s) { std::printf("%-58s %10.2f ms\n", what, s * 1000); }

static std::optional<IndexedFile> biggest(const GameIndex& gi, const char* ext) {
    std::optional<IndexedFile> best;
    for (const IndexedFile& f : gi.files()) {
        if (gi.extension(f) != ext || !gi.rank(f).active()) continue;
        if (!best || f.size > best->size) best = f;
    }
    return best;
}

static double psnr(const Image& a, const Image& b, bool alpha) {
    double se = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.rgba.size(); i++) {
        if (!alpha && i % 4 == 3) continue;
        const double d = (double)a.rgba[i] - b.rgba[i];
        se += d * d;
        n++;
    }
    return se == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / (se / (double)n));
}

int main(int argc, char** argv) {
    bool scan = false, encode = false, rpf = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--scan")) scan = true;
        else if (!std::strcmp(argv[i], "--encode")) encode = true;
        else if (!std::strcmp(argv[i], "--rpf")) rpf = true;
        else if (!std::strncmp(argv[i], "--runs=", 7)) g_runs = std::max(1, std::atoi(argv[i] + 7));
    }
    const std::string dir = GameIndex::default_game_dir();
    if (dir.empty()) { std::printf("no game install: skipped\n"); return 0; }

    if (scan) {
        GameIndexOptions o;
        o.rebuild = true;
        o.cache_folder = (std::filesystem::temp_directory_path() / "rk_bench_index").string();
        auto t0 = std::chrono::steady_clock::now();
        GameIndex g = GameIndex::open(dir, o);
        line("GameIndex::open, full scan", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        std::printf("  %zu archives, %zu files, %zu archetypes\n", g.archives().size(), g.files().size(), g.archetypes().size());
    }
    GameIndex gi = GameIndex::open(dir);
    line("GameIndex::open, from the cache", best_of([&] { GameIndex::open(dir); }));

    if (rpf) {
        // every archive the index knows opened (nested ones through their parents), and every entry checked to lie inside
        // its own archive
        size_t entries = 0, outside = 0, failed = 0;
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::optional<Rpf>> open(gi.archives().size());
        for (size_t i = 0; i < gi.archives().size(); i++) {
            const IndexedArchive& a = gi.archives()[i];
            try {
                if (a.parent < 0) open[i] = Rpf::open(dir + "/" + a.path);
                else if (open[(size_t)a.parent]) open[i] = open[(size_t)a.parent]->open_child((int)a.parent_entry);
            } catch (const Error& e) { failed++; std::printf("  %s: %s\n", a.path.c_str(), e.what()); }
            if (!open[i]) continue;
            const Rpf& r = *open[i];
            for (const RpfEntry& e : r.entries()) {
                if (e.is_directory()) continue;
                entries++;
                const uint64_t len = e.is_binary() ? (e.stored_size ? e.stored_size : e.uncompressed_size) : e.stored_size;
                if ((uint64_t)e.offset_blocks * 512 + len > r.byte_size()) {
                    if (outside++ < 10) std::printf("  outside its archive: %s/%s\n", a.path.c_str(), e.path.c_str());
                }
            }
        }
        line("open every archive (tables, names, nested archives)", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        std::printf("  %zu archives, %zu entries, %zu outside their archive, %zu archives failed\n", gi.archives().size(), entries, outside, failed);
    }

    if (auto f = biggest(gi, ".ytd")) {
        std::printf("ytd %s (%u bytes)\n", gi.file_name(*f).c_str(), f->size);
        Bytes b = gi.extract(*f);
        line("  extract", best_of([&] { gi.extract(*f); }));
        TextureDictionary t = read_ytd(b);
        line("  read_ytd", best_of([&] { read_ytd(b); }));
        line("  decode every level", best_of([&] {
            for (const Texture& x : t.textures)
                for (uint32_t l = 0; l < x.levels; l++) decode_texture(x, l);
        }));
        line("  write_ytd", best_of([&] { write_ytd(t, gi.info().game); }));
        {
            const rsc::Resource res = rsc::Resource::load(b);
            Bytes body = res.system;
            body.insert(body.end(), res.graphics.begin(), res.graphics.end());
            line("    of which deflating the pages", best_of([&] { rsc::deflate(body); }));
        }
        if (encode) {
            double sum1 = 0, sum5 = 0;
            int n = 0;
            const int runs = g_runs;
            g_runs = 1;
            double s1 = 0, s5 = 0;
            for (const Texture& x : t.textures) {
                // a level decoded from BC1 / BC3 sits on the block lattice the encoder can hit exactly; its half size (box
                // filtered) does not, so the PSNR there says how well the encoder fits colours it has to approximate
                Image img = decode_texture(x, 0);
                if (img.width >= 8 && img.height >= 8) img = downsample(img);
                if (img.width < 4 || img.height < 4) continue;
                Texture e1, e5;
                s1 += best_of([&] { e1 = encode_texture(img, TextureFormat::DXT1, "a", 1); });
                s5 += best_of([&] { e5 = encode_texture(img, TextureFormat::DXT5, "a", 1); });
                const double p1 = psnr(img, decode_texture(e1, 0), false), p5 = psnr(img, decode_texture(e5, 0), true);
                std::printf("  %-40s %4ux%-4u DXT1 %6.2f dB  DXT5 %6.2f dB\n", x.name.c_str(), img.width, img.height, p1, p5);
                sum1 += p1;
                sum5 += p5;
                n++;
            }
            g_runs = runs;
            if (n) std::printf("  mean PSNR over %d textures: DXT1 %.2f dB (rgb), DXT5 %.2f dB (rgba)\n", n, sum1 / n, sum5 / n);
            line("  encode DXT1, level 0 of every texture", s1);
            line("  encode DXT5, level 0 of every texture", s5);
        }
    }
    if (auto f = biggest(gi, ".ydr")) {
        std::printf("ydr %s (%u bytes)\n", gi.file_name(*f).c_str(), f->size);
        Bytes b = gi.extract(*f);
        Drawable d = read_ydr(b);
        line("  read_ydr", best_of([&] { read_ydr(b); }));
        line("  write_ydr", best_of([&] { write_ydr(d, gi.info().game); }));
    }
    if (auto f = biggest(gi, ".ybn")) {
        std::printf("ybn %s (%u bytes)\n", gi.file_name(*f).c_str(), f->size);
        Bytes b = gi.extract(*f);
        BoundPtr bd = read_ybn(b);
        line("  read_ybn", best_of([&] { read_ybn(b); }));
        line("  write_ybn", best_of([&] { write_ybn(*bd); }));
        line("  finalize (boxes, BVH) + write_ybn", best_of([&] { BoundPtr c = read_ybn(b); c->finalize(); write_ybn(*c); }) - best_of([&] { read_ybn(b); }));
    }
    return 0;
}
