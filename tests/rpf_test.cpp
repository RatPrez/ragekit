// rk_rpf_test: cipher test vectors, and (with an archive argument) every .ytd/.ydr/.ydd/.ybn/.yft/.ytyp/.ymap in it,
// nested archives included, is extracted from the RPF and parsed with ragekit's readers.
//   rk_rpf_test [<archive.rpf> ...]
#include <cstdio>
#include <cstring>
#include <string>
#include "ragekit/ragekit.h"
#include "rpf_crypto.h"

using namespace rk;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static bool ends(const std::string& s, const char* e) { size_t n = std::strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }

static int g_read = 0, g_failed = 0, g_skipped = 0;

static void walk(const Rpf& rpf, const std::string& where) {
    for (size_t i = 0; i < rpf.entries().size(); i++) {
        const RpfEntry& e = rpf.entries()[i];
        if (e.is_directory()) continue;
        std::string n = lower(e.name);
        try {
            if (e.is_archive()) { walk(rpf.open_child((int)i), where + e.path + "/"); continue; }
            if (!e.is_resource()) continue;
            Bytes data = rpf.extract((int)i);
            if (data.empty()) { g_skipped++; continue; }
            if (ends(n, ".ytd")) read_ytd(data);
            else if (ends(n, ".ydr")) read_ydr(data);
            else if (ends(n, ".ydd")) read_ydd(data);
            else if (ends(n, ".ybn")) read_ybn(data);
            else if (ends(n, ".yft")) read_yft(data);
            else if (ends(n, ".ytyp")) read_ytyp(data);
            else if (ends(n, ".ymap")) read_ymap(data);
            else continue;
            g_read++;
        } catch (const std::exception& ex) {
            g_failed++;
            std::printf("FAIL %s%s: %s\n", where.c_str(), e.path.c_str(), ex.what());
        }
    }
}

static Bytes bytes_of(const std::string& s) { return Bytes(s.begin(), s.end()); }

static Bytes fake_rsc7(size_t body, uint32_t sys, uint32_t gfx) {
    Bytes f(16 + body);
    uint32_t magic = 0x37435352, ver = 165;
    std::memcpy(f.data(), &magic, 4); std::memcpy(f.data() + 4, &ver, 4);
    std::memcpy(f.data() + 8, &sys, 4); std::memcpy(f.data() + 12, &gfx, 4);
    for (size_t i = 0; i < body; i++) f[16 + i] = (uint8_t)(i * 2654435761u >> 13);
    return f;
}

static void writer_tests() {
    // from scratch: directories, a compressed and a stored binary, resources (one over 16 MB), a nested archive
    RpfBuilder child;
    child.add_binary("inner/hello.txt", bytes_of("hello inner"), false);
    RpfBuilder b;
    Bytes text(5000, 'x');
    b.add_binary("common/data/a.meta", text, true);
    b.add_binary("Common/data/b.bin", bytes_of("stored"), false);
    Bytes rsc = fake_rsc7(3000, 0xA0000002u, 0x50000000u);
    b.add_resource("stream/prop.ydr", rsc);
    Bytes big = fake_rsc7(0x1100000, 0x00123456u, 0x00654321u);
    b.add_file("stream/big.ytd", big);
    b.add_archive("dlc/child.rpf", std::move(child));
    for (int enc = 0; enc < 2; enc++) {
        RpfWriteOptions opt;
        if (enc) {
            opt.encryption = RpfEncryption::Aes;
            if (!RpfKeys::default_keys()->has_aes()) { std::printf("skip AES writer case (no key)\n"); continue; }
        }
        Rpf r = Rpf::open_memory(b.build(opt), "t.rpf");
        const char* tag = enc ? "AES" : "OPEN";
        check(r.encryption() == (enc ? RpfEncryption::Aes : RpfEncryption::Open), tag);
        int i = r.find("common/data/a.meta");
        check(i >= 0 && r.extract(i) == text && r.entries()[(size_t)i].compressed() && r.entries()[(size_t)i].stored_size < 5000, "compressed binary round trip");
        i = r.find("COMMON/data/b.bin");
        check(i >= 0 && r.extract(i) == bytes_of("stored") && !r.entries()[(size_t)i].compressed(), "stored binary round trip");
        i = r.find("stream/prop.ydr");
        check(i >= 0 && r.extract(i) == rsc && r.entries()[(size_t)i].system_flags == 0xA0000002u && r.entries()[(size_t)i].graphics_flags == 0x50000000u, "resource keeps header and flags");
        i = r.find("stream/big.ytd");
        Bytes got = i >= 0 ? r.extract(i) : Bytes();
        check(i >= 0 && r.entries()[(size_t)i].stored_size == big.size() && got.size() == big.size() && std::memcmp(got.data() + 16, big.data() + 16, big.size() - 16) == 0,
              "resource over 16 MB (scrambled size)");
        auto loc = r.resolve("dlc/child.rpf/inner/hello.txt");
        check(loc.archive && loc.archive->extract(loc.entry) == bytes_of("hello inner"), "nested archive");
        // directory order: children sorted by name and contiguous
        bool sorted = true;
        for (size_t d = 0; d < r.entries().size(); d++) {
            if (!r.entries()[d].is_directory()) continue;
            auto kids = r.children((int)d);
            for (size_t k = 1; k < kids.size(); k++) if (!(r.entries()[(size_t)kids[k - 1]].name < r.entries()[(size_t)kids[k]].name)) sorted = false;
        }
        check(sorted, "children sorted by name");
    }
    // modify: from_archive, add, replace, rename, remove
    RpfBuilder m = RpfBuilder::from_archive(Rpf::open_memory(b.build(), "t.rpf"));
    m.add_binary("common/data/a.meta", bytes_of("replaced"), true);
    m.add_binary("new/file.txt", bytes_of("new"), false);
    check(m.rename("stream/prop.ydr", "prop2.ydr"), "rename");
    check(m.remove("common/data/b.bin") && !m.exists("common/data/b.bin"), "remove");
    check(m.remove("dlc") && !m.exists("dlc/child.rpf"), "remove directory");
    Rpf r2 = Rpf::open_memory(m.build(), "t.rpf");
    int i = r2.find("common/data/a.meta");
    check(i >= 0 && r2.extract(i) == bytes_of("replaced"), "replaced file");
    check(r2.find("stream/prop2.ydr") >= 0 && r2.find("stream/prop.ydr") < 0 && r2.find("new/file.txt") >= 0, "modified tree");
    i = r2.find("stream/big.ytd");
    check(i >= 0 && r2.extract(i).size() == big.size(), "untouched entry copied through");
}

int main(int argc, char** argv) {
    {
        uint8_t key[32], buf[16], plain[16];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
        for (int i = 0; i < 16; i++) plain[i] = (uint8_t)(i * 0x11);
        std::memcpy(buf, plain, 16);
        rpfcrypto::aes_encrypt(buf, 16, key);
        static const uint8_t expect[16] = {0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf, 0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89};
        check(std::memcmp(buf, expect, 16) == 0, "AES-256 encrypt (FIPS-197 C.3)");
        rpfcrypto::aes_decrypt(buf, 16, key);
        check(std::memcmp(buf, plain, 16) == 0, "AES-256 decrypt");
        uint8_t h[20];
        rpfcrypto::sha1((const uint8_t*)"abc", 3, h);
        static const uint8_t sha_abc[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e, 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
        check(std::memcmp(h, sha_abc, 20) == 0, "SHA1(abc)");
        Bytes big(1000, 'a');
        rpfcrypto::sha1(big.data(), big.size(), h);
        static const uint8_t sha_a1000[20] = {0x29, 0x1e, 0x9a, 0x6c, 0x66, 0x99, 0x49, 0x49, 0xb5, 0x7b, 0xa5, 0xe6, 0x50, 0x36, 0x1e, 0x98, 0xfc, 0x36, 0xb1, 0xba};
        check(std::memcmp(h, sha_a1000, 20) == 0, "SHA1(1000 x 'a')");
    }
    writer_tests();
    for (int i = 1; i < argc; i++) {
        g_read = g_failed = g_skipped = 0;
        Rpf rpf = Rpf::open(argv[i]);
        if (rpf.byte_size() < (64u << 20)) { // small enough to also open from memory
            Rpf mem = Rpf::open_memory(rsc::read_file(argv[i]), rpf.name());
            check(mem.entries().size() == rpf.entries().size(), "open_memory sees the same entries");
        }
        walk(rpf, "");
        std::printf("%s: %d resources parsed, %d failed, %d empty\n", argv[i], g_read, g_failed, g_skipped);
        failures += g_failed;
    }
    std::printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
