#include "ragekit/rpf.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "ragekit/error.h"
#include "ragekit/hash.h"
#include "ragekit/rsc.h"
#include "inflate_fast.h"
#include "miniz.h"
#include "rpf_crypto.h"

namespace rk {

namespace {
#include "rpf_key_hashes.inc"
Bytes inflate_auto(const uint8_t* data, size_t len, size_t hint);

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string slashes(std::string s) {
    for (auto& c : s) if (c == '\\') c = '/';
    return s;
}
bool ends_with(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && std::equal(s.end() - (ptrdiff_t)n, s.end(), suffix);
}
uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

// ---------------------------------------------------------------------------------------
// Keys

Bytes read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    f.seekg(0);
    Bytes b((size_t)n);
    if (n > 0) f.read((char*)b.data(), n);
    return b;
}
void write_all(const std::string& path, const void* data, size_t n) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw Error("cannot write " + path);
    f.write((const char*)data, (std::streamsize)n);
}

struct HashKey {
    uint64_t v;
    bool operator==(const HashKey& o) const { return v == o.v; }
};
struct HashKeyHasher { size_t operator()(const HashKey& k) const { return (size_t)k.v; } };

} // namespace

std::string RpfKeys::default_folder() {
    const char* home = std::getenv("USERPROFILE");
    if (!home || !*home) home = std::getenv("HOME");
    std::string base = home ? home : ".";
    return base + "/.ragekit/keys";
}

RpfKeys RpfKeys::load(const std::string& folder) {
    RpfKeys k;
    std::string dir = folder + "/";
    k.aes = read_all(dir + "gtav_aes_key.dat");
    k.ng_keys = read_all(dir + "gtav_ng_key.dat");
    Bytes t = read_all(dir + "gtav_ng_decrypt_tables.dat");
    if (t.size() == 17 * 16 * 256 * 4) {
        k.ng_tables.resize(17 * 16 * 256);
        std::memcpy(k.ng_tables.data(), t.data(), t.size());
    }
    k.hash_lut = read_all(dir + "gtav_hash_lut.dat");
    k.awc = read_all(dir + "gtav_awc_key.dat");
    return k;
}

void RpfKeys::save(const std::string& folder) const {
    std::string dir = folder + "/";
    if (has_aes()) write_all(dir + "gtav_aes_key.dat", aes.data(), aes.size());
    if (!ng_keys.empty()) write_all(dir + "gtav_ng_key.dat", ng_keys.data(), ng_keys.size());
    if (!ng_tables.empty()) write_all(dir + "gtav_ng_decrypt_tables.dat", ng_tables.data(), ng_tables.size() * 4);
    if (!hash_lut.empty()) write_all(dir + "gtav_hash_lut.dat", hash_lut.data(), hash_lut.size());
    if (has_awc()) write_all(dir + "gtav_awc_key.dat", awc.data(), awc.size());
}

namespace {
// .NET's seeded System.Random (the subtractive generator), which CodeWalker uses to obfuscate magic.dat.
struct DotnetRandom {
    int seed_array[56];
    int inext = 0, inextp = 21;
    explicit DotnetRandom(int seed) {
        int subtraction = seed == INT32_MIN ? INT32_MAX : std::abs(seed);
        int mj = 161803398 - subtraction;
        seed_array[55] = mj;
        int mk = 1;
        for (int i = 1; i < 55; i++) {
            int ii = (21 * i) % 55;
            seed_array[ii] = mk;
            mk = mj - mk;
            if (mk < 0) mk += INT32_MAX;
            mj = seed_array[ii];
        }
        for (int k = 1; k < 5; k++)
            for (int i = 1; i < 56; i++) {
                seed_array[i] -= seed_array[1 + (i + 30) % 55];
                if (seed_array[i] < 0) seed_array[i] += INT32_MAX;
            }
    }
    int sample() {
        if (++inext >= 56) inext = 1;
        if (++inextp >= 56) inextp = 1;
        int r = seed_array[inext] - seed_array[inextp];
        if (r == INT32_MAX) r--;
        if (r < 0) r += INT32_MAX;
        seed_array[inext] = r;
        return r;
    }
    void next_bytes(Bytes& b) { for (auto& x : b) x = (uint8_t)sample(); }
};

/// CodeWalker keeps the NG keys, decrypt tables and hash LUT in an embedded resource ("magic.dat"),
/// obfuscated with random streams seeded from the AES key, then AES encrypted and deflated.
bool decode_magic(const Bytes& magic, RpfKeys& k) {
    if (!k.has_aes() || magic.empty()) return false;
    DotnetRandom rnd((int)rk::joaat(std::string_view((const char*)k.aes.data(), 32)));
    size_t n = magic.size();
    Bytes rb[4];
    for (auto& r : rb) { r.resize(n); rnd.next_bytes(r); }
    Bytes db(n);
    for (size_t i = 0; i < n; i++) db[i] = (uint8_t)(magic[i] - rb[0][i] - rb[1][i] - rb[2][i] - rb[3][i]);
    rpfcrypto::aes_decrypt(db.data(), db.size(), k.aes.data());
    Bytes b = inflate_auto(db.data(), db.size(), 306272);
    if (b.size() < 27472 + 278528 + 256) return false;
    k.ng_keys.assign(b.begin(), b.begin() + 27472);
    k.ng_tables.resize(17 * 16 * 256);
    std::memcpy(k.ng_tables.data(), b.data() + 27472, 278528);
    k.hash_lut.assign(b.begin() + 27472 + 278528, b.begin() + 27472 + 278528 + 256);
    if (b.size() >= 27472 + 278528 + 256 + 16) k.awc.assign(b.begin() + 27472 + 278528 + 256, b.begin() + 27472 + 278528 + 256 + 16);
    return true;
}
} // namespace

RpfKeys RpfKeys::generate(const std::string& exe_path, const std::function<void(const std::string&)>& progress,
                          const std::string& magic_path) {
    Bytes exe = read_all(exe_path);
    if (exe.empty()) throw Error("cannot read " + exe_path);
    auto say = [&](const std::string& s) { if (progress) progress(s); };

    auto key_of = [](const uint8_t* h) { uint64_t v; std::memcpy(&v, h, 8); return HashKey{v}; };
    std::unordered_map<HashKey, int, HashKeyHasher> ng_map, tab_map;
    for (int i = 0; i < 101; i++) ng_map[key_of(kNgKeyHashes[i])] = i;
    for (int i = 0; i < 272; i++) tab_map[key_of(kNgTableHashes[i])] = i;

    RpfKeys out;
    out.ng_keys.assign(101 * 272, 0);
    out.ng_tables.assign(17 * 16 * 256, 0);
    std::vector<std::atomic<bool>> ng_found(101), tab_found(272);
    std::atomic<bool> aes_found{false}, lut_found{false};
    std::mutex mu;

    const size_t kAlign = 8;
    size_t positions = exe.size() / kAlign;
    unsigned nthreads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
    const size_t chunk = 1 << 16;
    auto worker = [&]() {
        for (;;) {
            size_t start = next.fetch_add(chunk);
            if (start >= positions) break;
            size_t end = std::min(positions, start + chunk);
            for (size_t i = start; i < end; i++) {
                size_t pos = i * kAlign;
                const uint8_t* p = exe.data() + pos;
                size_t left = exe.size() - pos;
                uint8_t h[20];
                if (left >= 32) {
                    rpfcrypto::sha1(p, 32, h);
                    if (std::memcmp(h, kAesKeyHash, 20) == 0) {
                        std::lock_guard<std::mutex> g(mu);
                        out.aes.assign(p, p + 32);
                        aes_found = true;
                    }
                }
                if (left >= 272) {
                    rpfcrypto::sha1(p, 272, h);
                    auto it = ng_map.find(key_of(h));
                    if (it != ng_map.end() && std::memcmp(h, kNgKeyHashes[it->second], 20) == 0) {
                        std::lock_guard<std::mutex> g(mu);
                        std::memcpy(out.ng_keys.data() + (size_t)it->second * 272, p, 272);
                        ng_found[it->second] = true;
                    }
                }
                if (left >= 1024) {
                    rpfcrypto::sha1(p, 1024, h);
                    auto it = tab_map.find(key_of(h));
                    if (it != tab_map.end() && std::memcmp(h, kNgTableHashes[it->second], 20) == 0) {
                        std::lock_guard<std::mutex> g(mu);
                        std::memcpy(out.ng_tables.data() + (size_t)it->second * 256, p, 1024);
                        tab_found[it->second] = true;
                    }
                }
                if (left >= 256) {
                    rpfcrypto::sha1(p, 256, h);
                    if (std::memcmp(h, kHashLutHash, 20) == 0) {
                        std::lock_guard<std::mutex> g(mu);
                        out.hash_lut.assign(p, p + 256);
                        lut_found = true;
                    }
                }
            }
            done += end - start;
        }
    };
    say("Searching " + exe_path + " (" + std::to_string(exe.size() >> 20) + " MB, " + std::to_string(nthreads) + " threads)...");
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < nthreads; t++) threads.emplace_back(worker);
    size_t last_pct = 0;
    for (auto& t : threads) t.join();
    (void)last_pct;

    int ng_n = 0, tab_n = 0;
    for (auto& f : ng_found) ng_n += f ? 1 : 0;
    for (auto& f : tab_found) tab_n += f ? 1 : 0;
    say(std::string("AES key ") + (aes_found ? "found" : "NOT found") + ", NG keys " + std::to_string(ng_n) + "/101, NG tables " +
        std::to_string(tab_n) + "/272, hash LUT " + (lut_found ? "found" : "NOT found"));
    if (!aes_found) throw Error("the AES key was not found in " + exe_path + " (not a GTA V executable this tool knows?)");
    if (ng_n != 101 || tab_n != 272 || !lut_found) {
        out.ng_keys.clear();
        out.ng_tables.clear();
        if (!lut_found) out.hash_lut.clear();
        // Newer executables only hold the AES key: the NG data comes from CodeWalker's magic.dat.
        if (!magic_path.empty()) {
            if (!decode_magic(read_all(magic_path), out)) throw Error("cannot decode the NG data from " + magic_path);
            say("NG keys, tables and hash LUT decoded from " + magic_path);
        } else {
            out.hash_lut.clear();
            say("NG data is not in this executable: pass CodeWalker's magic.dat (CodeWalker.Core/Resources) to get it. Only AES archives will open");
        }
    }
    return out;
}

std::shared_ptr<const RpfKeys> RpfKeys::default_keys() {
    static std::shared_ptr<const RpfKeys> keys = std::make_shared<RpfKeys>(load(default_folder()));
    return keys;
}

// ---------------------------------------------------------------------------------------
// Entries

bool RpfEntry::is_archive() const {
    return kind == Kind::Binary && ends_with(lower(name), ".rpf");
}
uint64_t RpfEntry::size() const {
    if (kind == Kind::Binary) return uncompressed_size ? uncompressed_size : stored_size;
    if (kind == Kind::Resource) return system_size() + graphics_size();
    return 0;
}
uint64_t RpfEntry::system_size() const { return rsc::PageFlags{system_flags}.total_size(); }
uint64_t RpfEntry::graphics_size() const { return rsc::PageFlags{graphics_flags}.total_size(); }

// ---------------------------------------------------------------------------------------
// Archive

namespace {
/// Bytes of an archive's storage: a file or memory, shared with nested archives.
struct Source {
    std::string path; // empty for memory
    Bytes memory;
    uint64_t size = 0;
    std::mutex mu;
    std::ifstream file;

    void read(uint64_t offset, size_t len, uint8_t* out) {
        if (offset + len > size) throw Error("read past the end of the archive");
        if (path.empty()) {
            std::memcpy(out, memory.data() + offset, len);
            return;
        }
        std::lock_guard<std::mutex> g(mu);
        file.clear();
        file.seekg((std::streamoff)offset);
        file.read((char*)out, (std::streamsize)len);
        if ((size_t)file.gcount() != len) throw Error("short read from " + path);
    }
};

Bytes inflate_auto(const uint8_t* data, size_t len, size_t hint) {
    // the hint is the entry's own size word: never more than the stream could give (1032 bytes per byte of deflate)
    Bytes out(std::min<size_t>(std::max<size_t>(hint, 64), (size_t)std::min<uint64_t>((uint64_t)len * 1032 + 0x10000, SIZE_MAX)));
    // a whole stream that fits the hint with the table-driven decoder; anything else (larger, truncated, corrupt) with miniz
    size_t n = 0;
    if (rk::detail::inflate_fast(data, len, out.data(), out.size(), n)) {
        out.resize(n);
        return out;
    }
    mz_stream zs{};
    if (mz_inflateInit2(&zs, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) throw Error("inflateInit2 failed");
    zs.next_in = data;
    zs.avail_in = (unsigned)len;
    for (;;) {
        zs.next_out = out.data() + zs.total_out;
        zs.avail_out = (unsigned)(out.size() - zs.total_out);
        int rc = mz_inflate(&zs, MZ_NO_FLUSH);
        if (rc == MZ_STREAM_END) break;
        if (rc == MZ_OK || rc == MZ_BUF_ERROR) {
            if (zs.avail_out == 0) { out.resize(out.size() * 2); continue; }
            if (zs.avail_in == 0) break; // truncated stream: keep what we have
            continue;
        }
        mz_inflateEnd(&zs);
        throw Error("inflate failed: corrupt deflate stream");
    }
    out.resize(zs.total_out);
    mz_inflateEnd(&zs);
    return out;
}
} // namespace

struct Rpf::Impl {
    std::shared_ptr<Source> src;
    uint64_t start = 0, size = 0;
    std::string name;
    std::shared_ptr<const RpfKeys> keys;
    RpfEncryption encryption = RpfEncryption::None;
    bool aes = false;
    uint32_t names_length = 0;
    std::vector<RpfEntry> entries;
    std::unordered_map<std::string, int> by_path; // lowercase path

    const RpfKeys& need_keys(bool ng) const {
        const RpfKeys& k = *keys;
        if (ng ? !k.has_ng() : !k.has_aes())
            throw Error(std::string("'") + name + "' is " + (ng ? "NG" : "AES") + " encrypted and no " + (ng ? "NG" : "AES") +
                        " keys are available (see RpfKeys: " + RpfKeys::default_folder() + ")");
        return k;
    }
    void decrypt_header(Bytes& b, uint32_t length) const {
        if (encryption == RpfEncryption::None || encryption == RpfEncryption::Open) return;
        if (encryption == RpfEncryption::Aes) {
            rpfcrypto::aes_decrypt(b.data(), b.size(), need_keys(false).aes.data());
        } else { // Ng, and unknown values are taken as NG like CodeWalker does
            const RpfKeys& k = need_keys(true);
            rpfcrypto::ng_decrypt(b.data(), b.size(), name, length, k.ng_keys.data(), k.ng_tables.data(), k.hash_lut.data());
        }
    }
    /// Where an entry's `len` bytes start in the source; throws (before anything of that size is allocated) when they run
    /// past this archive, whose extent inside a parent archive is all the entry may use.
    uint64_t entry_at(const RpfEntry& e, uint64_t len) const {
        const uint64_t off = (uint64_t)e.offset_blocks * 512;
        if (off > size || len > size - off) throw Error("'" + e.path + "' runs past the end of its archive");
        return start + off;
    }
    void decrypt_file(Bytes& b, const std::string& entry_name, uint32_t length) const {
        if (aes) {
            rpfcrypto::aes_decrypt(b.data(), b.size(), need_keys(false).aes.data());
        } else { // anything not AES is taken as NG (OpenIV modded files)
            const RpfKeys& k = need_keys(true);
            rpfcrypto::ng_decrypt(b.data(), b.size(), entry_name, length, k.ng_keys.data(), k.ng_tables.data(), k.hash_lut.data());
        }
    }
};

static std::shared_ptr<Rpf::Impl> read_archive(std::shared_ptr<Source> src, uint64_t start, uint64_t size, const std::string& name,
                                               std::shared_ptr<const RpfKeys> keys) {
    auto d = std::make_shared<Rpf::Impl>();
    d->src = std::move(src);
    d->start = start;
    d->size = size;
    d->name = name;
    d->keys = keys ? std::move(keys) : RpfKeys::default_keys();

    if (size < 16) throw Error("'" + name + "' is too small to be an RPF");
    uint8_t head[16];
    d->src->read(start, 16, head);
    if (rd32(head) != 0x52504637) throw Error("'" + name + "' is not an RPF7 archive (GTA V)");
    uint32_t count = rd32(head + 4), names_len = rd32(head + 8);
    d->encryption = (RpfEncryption)rd32(head + 12);
    d->aes = d->encryption == RpfEncryption::Aes;
    d->names_length = names_len;
    if ((uint64_t)count * 16 + names_len + 16 > size) throw Error("'" + name + "' has a table larger than the archive");

    Bytes table((size_t)count * 16), names(names_len);
    if (!table.empty()) d->src->read(start + 16, table.size(), table.data());
    if (!names.empty()) d->src->read(start + 16 + table.size(), names.size(), names.data());
    d->decrypt_header(table, (uint32_t)size);
    d->decrypt_header(names, (uint32_t)size);

    d->entries.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* p = table.data() + (size_t)i * 16;
        uint32_t y = rd32(p), x = rd32(p + 4);
        RpfEntry& e = d->entries[i];
        if (x == 0x7fffff00) {
            e.kind = RpfEntry::Kind::Directory;
            e.name_offset = y;
            e.first_child = rd32(p + 8);
            e.child_count = rd32(p + 12);
        } else if ((x & 0x80000000) == 0) {
            e.kind = RpfEntry::Kind::Binary;
            uint64_t buf;
            std::memcpy(&buf, p, 8);
            e.name_offset = (uint32_t)buf & 0xFFFF;
            e.stored_size = (uint32_t)(buf >> 16) & 0xFFFFFF;
            e.offset_blocks = (uint32_t)(buf >> 40) & 0xFFFFFF;
            e.uncompressed_size = rd32(p + 8);
            uint32_t enc = rd32(p + 12);
            if (enc > 1) throw Error("'" + name + "': error in RPF7 file entry " + std::to_string(i));
            e.encrypted = enc == 1;
        } else {
            e.kind = RpfEntry::Kind::Resource;
            e.name_offset = (uint32_t)p[0] | (uint32_t)p[1] << 8;
            e.stored_size = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16;
            e.offset_blocks = ((uint32_t)p[5] | (uint32_t)p[6] << 8 | (uint32_t)p[7] << 16) & 0x7FFFFF;
            e.system_flags = rd32(p + 8);
            e.graphics_flags = rd32(p + 12);
            if (e.stored_size == 0xFFFFFF) {
                // sizes of 0xFFFFFF mean "at least"; the real size is scrambled into the file's 16 byte header
                uint8_t h[16];
                d->src->read(start + (uint64_t)e.offset_blocks * 512, 16, h);
                e.stored_size = (uint32_t)h[7] | (uint32_t)h[14] << 8 | (uint32_t)h[5] << 16 | (uint32_t)h[2] << 24;
            }
        }
        if (e.name_offset < names.size()) {
            const char* s = (const char*)names.data() + e.name_offset;
            size_t n = 0;
            while (e.name_offset + n < names.size() && s[n]) n++;
            e.name.assign(s, std::min<size_t>(n, 256));
        }
        if (e.kind == RpfEntry::Kind::Resource) e.encrypted = ends_with(lower(e.name), ".ysc");
    }

    if (count == 0 || !d->entries[0].is_directory()) throw Error("'" + name + "' has no root directory");
    // parents and paths, walking down from the root (guarded against cycles / out of range children)
    std::vector<int> stack{0};
    std::vector<bool> seen(count, false);
    seen[0] = true;
    while (!stack.empty()) {
        int di = stack.back();
        stack.pop_back();
        const RpfEntry dir = d->entries[di];
        uint64_t end = (uint64_t)dir.first_child + dir.child_count;
        if (end > count) throw Error("'" + name + "': directory '" + dir.name + "' lists entries past the table");
        for (uint32_t c = dir.first_child; c < end; c++) {
            if (seen[c]) continue;
            seen[c] = true;
            RpfEntry& e = d->entries[c];
            e.parent = di;
            e.path = dir.path.empty() ? e.name : dir.path + "/" + e.name;
            if (e.is_directory()) stack.push_back((int)c);
        }
    }
    for (uint32_t i = 0; i < count; i++) d->by_path.emplace(lower(d->entries[i].path), (int)i);
    return d;
}

Rpf Rpf::open(const std::string& path, std::shared_ptr<const RpfKeys> keys) {
    auto src = std::make_shared<Source>();
    src->path = path;
    src->file.open(path, std::ios::binary);
    if (!src->file) throw Error("cannot open " + path);
    src->file.seekg(0, std::ios::end);
    src->size = (uint64_t)src->file.tellg();
    size_t cut = path.find_last_of("/\\");
    std::string name = cut == std::string::npos ? path : path.substr(cut + 1);
    Rpf r;
    uint64_t sz = src->size;
    r.d_ = read_archive(std::move(src), 0, sz, name, std::move(keys));
    return r;
}

Rpf Rpf::open_memory(Bytes data, const std::string& name, std::shared_ptr<const RpfKeys> keys) {
    auto src = std::make_shared<Source>();
    src->size = data.size();
    src->memory = std::move(data);
    Rpf r;
    uint64_t sz = src->size;
    r.d_ = read_archive(std::move(src), 0, sz, name, std::move(keys));
    return r;
}

const std::string& Rpf::name() const { return d_->name; }
uint64_t Rpf::byte_size() const { return d_->size; }
RpfEncryption Rpf::encryption() const { return d_->encryption; }
uint32_t Rpf::names_length() const { return d_->names_length; }
const std::vector<RpfEntry>& Rpf::entries() const { return d_->entries; }

std::vector<int> Rpf::children(int dir) const {
    std::vector<int> out;
    const RpfEntry& e = d_->entries.at((size_t)dir);
    if (!e.is_directory()) return out;
    // a directory the root never reaches may list entries past the table (read_archive checks only the reachable ones)
    const uint64_t end = std::min<uint64_t>((uint64_t)e.first_child + e.child_count, d_->entries.size());
    for (uint64_t i = e.first_child; i < end; i++) out.push_back((int)i);
    return out;
}

int Rpf::find(const std::string& path) const {
    std::string p = lower(slashes(path));
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    while (!p.empty() && p.back() == '/') p.pop_back();
    auto it = d_->by_path.find(p);
    return it == d_->by_path.end() ? -1 : it->second;
}

Bytes Rpf::extract_raw(int entry) const {
    const RpfEntry& e = d_->entries.at((size_t)entry);
    if (e.is_directory()) throw Error("'" + e.path + "' is a directory");
    uint64_t len = e.is_binary() ? (e.stored_size ? e.stored_size : e.uncompressed_size) : e.stored_size;
    const uint64_t at = d_->entry_at(e, len);
    Bytes b((size_t)len);
    if (len) d_->src->read(at, (size_t)len, b.data());
    return b;
}

uint64_t Rpf::stored_length(int entry) const {
    const RpfEntry& e = d_->entries.at((size_t)entry);
    if (e.is_directory()) return 0;
    return e.is_binary() ? (e.stored_size ? e.stored_size : e.uncompressed_size) : e.stored_size;
}

Bytes Rpf::read_stored(int entry, uint64_t offset, size_t length) const {
    const RpfEntry& e = d_->entries.at((size_t)entry);
    if (e.is_directory()) throw Error("'" + e.path + "' is a directory");
    const uint64_t len = stored_length(entry);
    const uint64_t at = d_->entry_at(e, len);
    if (offset >= len) return {};
    Bytes b((size_t)std::min<uint64_t>(length, len - offset));
    if (!b.empty()) d_->src->read(at + offset, b.size(), b.data());
    return b;
}

Bytes Rpf::extract(int entry) const { return extract(d_->entries.at((size_t)entry)); }

Bytes Rpf::extract(const RpfEntry& e) const {
    if (e.is_directory()) throw Error("'" + e.path + "' is a directory");
    try {
        if (e.is_binary()) {
            uint64_t len = e.stored_size ? e.stored_size : e.uncompressed_size;
            if (len == 0) return {};
            const uint64_t at = d_->entry_at(e, len);
            Bytes b((size_t)len);
            d_->src->read(at, b.size(), b.data());
            if (e.encrypted) d_->decrypt_file(b, e.name, e.uncompressed_size);
            if (e.stored_size > 0) return inflate_auto(b.data(), b.size(), e.uncompressed_size);
            return b;
        }
        // resource: 16 byte header on disk, then the (maybe encrypted) deflate stream; the stream is read in place behind
        // the header the entry's own flags make, and decrypted there
        if (e.stored_size == 0) return {};
        if (e.stored_size < 16) throw Error("resource smaller than its header");
        const uint64_t at = d_->entry_at(e, e.stored_size);
        Bytes out(e.stored_size);
        d_->src->read(at + 16, out.size() - 16, out.data() + 16);
        if (e.encrypted) {
            Bytes body(out.begin() + 16, out.end());
            d_->decrypt_file(body, e.name, e.stored_size);
            std::memcpy(out.data() + 16, body.data(), body.size());
        }
        uint32_t magic = rsc::kMagic, version = e.resource_version();
        std::memcpy(out.data(), &magic, 4);
        std::memcpy(out.data() + 4, &version, 4);
        std::memcpy(out.data() + 8, &e.system_flags, 4);
        std::memcpy(out.data() + 12, &e.graphics_flags, 4);
        return out;
    } catch (const Error& err) {
        throw Error(d_->name + ":" + e.path + ": " + err.what());
    }
}

Bytes Rpf::extract_stored(int entry) const {
    const RpfEntry& e = d_->entries.at((size_t)entry);
    if (!e.is_binary()) return extract(entry);
    uint64_t len = e.stored_size ? e.stored_size : e.uncompressed_size;
    if (len == 0) return {};
    const uint64_t at = d_->entry_at(e, len);
    Bytes b((size_t)len);
    d_->src->read(at, b.size(), b.data());
    if (e.encrypted) d_->decrypt_file(b, e.name, e.uncompressed_size);
    return b;
}

Rpf Rpf::open_child(int entry) const {
    const RpfEntry& e = d_->entries.at((size_t)entry);
    if (!e.is_binary()) throw Error("'" + e.path + "' is not a binary file");
    uint64_t size = e.stored_size ? e.stored_size : e.uncompressed_size;
    uint64_t start = d_->start + (uint64_t)e.offset_blocks * 512;
    if (start + size > d_->start + d_->size) throw Error("'" + e.path + "' lies outside its archive");
    Rpf r;
    r.d_ = read_archive(d_->src, start, size, e.name, d_->keys);
    return r;
}

Rpf::Location Rpf::resolve(const std::string& path) const {
    std::string p = slashes(path);
    Location loc;
    Rpf cur = *this;
    size_t pos = 0;
    // try the longest prefix inside this archive, then descend through the nested archive
    for (;;) {
        std::string rest = p.substr(pos);
        while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
        int idx = cur.find(rest);
        if (idx >= 0) {
            loc.archive = std::make_shared<Rpf>(cur);
            loc.entry = idx;
            return loc;
        }
        // find a prefix of `rest` ending in .rpf that names an archive entry
        bool descended = false;
        size_t cut = 0;
        while ((cut = rest.find('/', cut)) != std::string::npos) {
            std::string prefix = rest.substr(0, cut);
            int ai = cur.find(prefix);
            if (ai >= 0 && cur.entries()[(size_t)ai].is_archive()) {
                cur = cur.open_child(ai);
                pos = (size_t)(p.size() - rest.size() + cut + 1);
                descended = true;
                break;
            }
            cut++;
        }
        if (!descended) return loc;
    }
}

} // namespace rk
