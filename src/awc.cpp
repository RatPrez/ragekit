#include "ragekit/awc.h"
#include <algorithm>
#include <cstring>
#include "ragekit/error.h"

namespace rk {

namespace {

constexpr uint32_t kMagic = 0x54414441;   // "ADAT"

template <class T> T get(std::span<const uint8_t> b, size_t at) {
    if (at + sizeof(T) > b.size()) throw Error("awc: the file ends inside its table (at byte " + std::to_string(at) + ")");
    T v;
    std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}
template <class T> void put(Bytes& b, size_t at, T v) { std::memcpy(b.data() + at, &v, sizeof(T)); }

const uint32_t* awc_key(const std::shared_ptr<const RpfKeys>& keys) {
    std::shared_ptr<const RpfKeys> k = keys ? keys : RpfKeys::default_keys();
    if (!k || !k->has_awc()) throw Error("awc: the file is encrypted and there is no AWC key (gtav_awc_key.dat: rk rpf keys <exe> --magic <magic.dat>)");
    static thread_local uint32_t key[4];
    std::memcpy(key, k->awc.data(), 16);
    return key;
}

// Rockstar's XXTEA: the reference algorithm with the key word xored with 0x7B3A207F.
void xxtea_decrypt(uint8_t* data, size_t len, const uint32_t key[4]) {
    const size_t n = len / 4;
    if (n < 2) return;
    std::vector<uint32_t> v(n);
    std::memcpy(v.data(), data, n * 4);
    uint32_t a, b = v[0];
    uint32_t i = (uint32_t)(0x9E3779B9u * (uint32_t)(6 + 52 / n));
    do {
        for (size_t k = n; k-- > 0;) {
            a = v[(k > 0 ? k : n) - 1];
            b = v[k] -= ((a >> 5 ^ b << 2) + (b >> 3 ^ a << 4)) ^ ((i ^ b) + (key[(k & 3) ^ (i >> 2 & 3)] ^ a ^ 0x7B3A207F));
        }
        i -= 0x9E3779B9u;
    } while (i != 0);
    std::memcpy(data, v.data(), n * 4);
}

void xxtea_encrypt(uint8_t* data, size_t len, const uint32_t key[4]) {
    const size_t n = len / 4;
    if (n < 2) return;
    std::vector<uint32_t> v(n);
    std::memcpy(v.data(), data, n * 4);
    int rounds = (int)(6 + 52 / n);
    uint32_t sum = 0, y, z = v[n - 1];
    do {
        sum += 0x9E3779B9u;
        const uint32_t e = (sum >> 2) & 3;
        for (size_t p = 0; p < n; p++) {
            y = v[(p + 1) % n];
            z = v[p] += ((z >> 5 ^ y << 2) + (y >> 3 ^ z << 4)) ^ ((sum ^ y) + (key[(p & 3) ^ e] ^ z ^ 0x7B3A207F));
        }
    } while (--rounds != 0);
    std::memcpy(data, v.data(), n * 4);
}

size_t table_size(const AwcFile& a) {
    size_t n = 16 + (a.flags & 1 ? 2 * a.streams.size() : 0) + 4 * a.streams.size();
    for (const AwcStream& s : a.streams) n += 8 * s.chunks.size();
    return n;
}

int sort_order(uint8_t t) {
    switch (t) {
        case awc_chunk::Data: case awc_chunk::Mid: return 3;
        case awc_chunk::Markers: case awc_chunk::GranularGrains: case awc_chunk::GranularLoops: case awc_chunk::Animation: case awc_chunk::Gesture: return 2;
        case awc_chunk::SeekTable: return 1;
        default: return 0;
    }
}
size_t alignment(uint8_t t) {
    switch (t) {
        case awc_chunk::Data: case awc_chunk::Mid: return 16;
        case awc_chunk::Markers: case awc_chunk::GranularGrains: case awc_chunk::GranularLoops: return 4;
        default: return 1;
    }
}

// IMA ADPCM as the game stores it: blocks of 2048 bytes, each a 4-byte header (step index, 0, predictor:i16) and 2044
// bytes of nibbles, low nibble first.
std::vector<int16_t> decode_adpcm(std::span<const uint8_t> data, uint32_t samples) {
    static const int index_table[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
    static const int16_t step_table[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
        190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
        18500, 20350, 22385, 24623, 27086, 29794, 32767};
    std::vector<int16_t> out;
    out.reserve(std::min<size_t>(samples, data.size() * 2));
    int predictor = 0, index = 0;
    size_t in_block = 0;
    auto nibble = [&](uint8_t n) {
        const int step = step_table[index];
        int diff = (((n & 7) << 1) + 1) * step >> 3;
        if (n & 8) diff = -diff;
        predictor += diff;
        index = std::clamp(index + index_table[n], 0, 88);
        out.push_back((int16_t)std::clamp(predictor, -32768, 32767));
    };
    for (size_t p = 0; p < data.size() && out.size() < samples;) {
        if (in_block == 0) {
            if (p + 4 > data.size()) break;
            index = std::clamp((int)data[p], 0, 88);
            int16_t pr;
            std::memcpy(&pr, data.data() + p + 2, 2);
            predictor = pr;
            in_block = 2044;
            p += 4;
        } else {
            nibble(data[p] & 0x0F);
            if (out.size() < samples) nibble(data[p] >> 4);
            in_block--;
            p++;
        }
    }
    return out;
}

std::vector<int16_t> decode(std::span<const uint8_t> data, uint8_t codec, uint32_t samples) {
    if (codec == 0) {
        std::vector<int16_t> pcm(std::min<size_t>(samples, data.size() / 2));
        if (!pcm.empty()) std::memcpy(pcm.data(), data.data(), pcm.size() * 2);
        return pcm;
    }
    if (codec == 4) return decode_adpcm(data, samples);
    return {};   // another codec: the sound is listed, its samples are not decoded
}

}  // namespace

const AwcChunk* AwcStream::chunk(uint8_t type) const {
    for (const AwcChunk& c : chunks) if (c.type == type) return &c;
    return nullptr;
}

bool is_awc(std::span<const uint8_t> file) {
    if (file.size() < 16) return false;
    uint32_t m;
    std::memcpy(&m, file.data(), 4);
    return m == kMagic || file.size() % 4 == 0;
}

AwcFile read_awc(std::span<const uint8_t> file, std::shared_ptr<const RpfKeys> keys) {
    if (file.size() < 16) throw Error("awc: a file of " + std::to_string(file.size()) + " bytes");
    AwcFile a;
    a.size = (uint32_t)file.size();
    Bytes plain;
    if (get<uint32_t>(file, 0) != kMagic) {
        if (get<uint32_t>(file, 0) == 0x41444154) throw Error("awc: a big-endian (console) file");
        if (file.size() % 4) throw Error("awc: no ADAT magic, and not a whole encrypted file (its size is not a multiple of 4)");
        plain.assign(file.begin(), file.end());
        xxtea_decrypt(plain.data(), plain.size(), awc_key(keys));
        if (get<uint32_t>(plain, 0) != kMagic) throw Error("awc: no ADAT magic, neither as stored nor decrypted");
        a.whole_file_encrypted = true;
        file = plain;
    }
    a.version = get<uint16_t>(file, 4);
    a.flags = get<uint16_t>(file, 6);
    const uint32_t count = get<uint32_t>(file, 8);
    if (count > file.size() / 4) throw Error("awc: " + std::to_string(count) + " streams do not fit in the file");
    size_t p = 16;
    if (a.flags & 1) {
        a.chunk_indices.resize(count);
        for (uint32_t i = 0; i < count; i++, p += 2) a.chunk_indices[i] = get<uint16_t>(file, p);
    }
    a.streams.resize(count);
    std::vector<uint32_t> nchunks(count);
    for (uint32_t i = 0; i < count; i++, p += 4) {
        const uint32_t v = get<uint32_t>(file, p);
        a.streams[i].id = v & 0x1FFFFFFF;
        nchunks[i] = v >> 29;
    }
    for (uint32_t i = 0; i < count; i++)
        for (uint32_t k = 0; k < nchunks[i]; k++, p += 8) {
            const uint64_t v = get<uint64_t>(file, p);
            AwcChunk c;
            c.type = (uint8_t)(v >> 56);
            c.offset = (uint32_t)(v & 0x0FFFFFFF);
            const uint32_t size = (uint32_t)((v >> 28) & 0x0FFFFFFF);
            if ((uint64_t)c.offset + size > file.size())
                throw Error("awc: a chunk of " + std::to_string(size) + " bytes at " + std::to_string(c.offset) + " runs past the file's " + std::to_string(file.size()));
            c.data.assign(file.begin() + c.offset, file.begin() + c.offset + size);
            a.streams[i].chunks.push_back(std::move(c));
        }
    if (get<uint32_t>(file, 12) != p) throw Error("awc: the chunk data is said to start at " + std::to_string(get<uint32_t>(file, 12)) + ", the table ends at " + std::to_string(p));
    return a;
}

void AwcFile::pack() {
    size_t at = table_size(*this);
    for (int order = 0; order <= 3; order++)
        for (AwcStream& s : streams)
            for (AwcChunk& c : s.chunks) {
                if (sort_order(c.type) != order) continue;
                const size_t al = alignment(c.type);
                at = (at + al - 1) / al * al;
                c.offset = (uint32_t)at;
                at += c.data.size();
            }
    size = (uint32_t)at;
}

Bytes write_awc(const AwcFile& a, std::shared_ptr<const RpfKeys> keys) {
    if (!a.chunk_indices.empty() && a.chunk_indices.size() != a.streams.size()) throw Error("awc: chunk indices and streams differ in count");
    const size_t table = table_size(a);
    size_t end = std::max<size_t>(a.size, table);
    for (const AwcStream& s : a.streams) {
        if (s.chunks.size() > 7) throw Error("awc: a stream with more than 7 chunks");
        for (const AwcChunk& c : s.chunks) {
            if (c.offset < table && !c.data.empty()) throw Error("awc: a chunk lies inside the table (call pack() after adding streams or chunks)");
            if (c.offset > 0x0FFFFFFF || c.data.size() > 0x0FFFFFFF) throw Error("awc: a chunk past 256 MB");
            end = std::max<size_t>(end, c.offset + c.data.size());
        }
    }
    if (a.whole_file_encrypted) end = (end + 3) / 4 * 4;
    Bytes b(end, 0);
    put(b, 0, kMagic);
    put(b, 4, a.version);
    put(b, 6, a.flags);
    put(b, 8, (uint32_t)a.streams.size());
    put(b, 12, (uint32_t)table);
    size_t p = 16;
    if (a.flags & 1)
        for (size_t i = 0; i < a.streams.size(); i++, p += 2) put(b, p, (uint16_t)(i < a.chunk_indices.size() ? a.chunk_indices[i] : 0));
    for (const AwcStream& s : a.streams) { put(b, p, (uint32_t)((s.id & 0x1FFFFFFF) | (uint32_t)s.chunks.size() << 29)); p += 4; }
    for (const AwcStream& s : a.streams)
        for (const AwcChunk& c : s.chunks) {
            put(b, p, (uint64_t)c.type << 56 | (uint64_t)c.data.size() << 28 | c.offset);
            p += 8;
            if (!c.data.empty()) std::memcpy(b.data() + c.offset, c.data.data(), c.data.size());
        }
    if (a.whole_file_encrypted) xxtea_encrypt(b.data(), b.size(), awc_key(keys));
    return b;
}

std::vector<AwcAudio> awc_decode(const AwcFile& a, std::shared_ptr<const RpfKeys> keys) {
    std::vector<AwcAudio> out;
    for (const AwcStream& s : a.streams) {
        const AwcChunk* data = s.chunk(awc_chunk::Data);
        if (!data) continue;
        if (const AwcChunk* sf = s.chunk(awc_chunk::StreamFormat); sf && a.multi_channel()) {
            // a multi-channel stream: blocks of `block_size` bytes, each with every channel's part of it
            if (sf->data.size() < 12) throw Error("awc: a stream format chunk of " + std::to_string(sf->data.size()) + " bytes");
            uint32_t blocks, block_size, channels;
            std::memcpy(&blocks, sf->data.data(), 4);
            std::memcpy(&block_size, sf->data.data() + 4, 4);
            std::memcpy(&channels, sf->data.data() + 8, 4);
            if (12 + (size_t)channels * 16 > sf->data.size()) throw Error("awc: the stream format lists more channels than it holds");
            std::vector<Bytes> chan(channels);
            bool decodable = true;   // Enhanced's codec 7 lays its blocks out otherwise: listed, not decoded
            for (uint32_t c = 0; c < channels; c++) {
                const uint8_t codec = sf->data[12 + c * 16 + 12];
                decodable &= codec == 0 || codec == 4;
            }
            for (uint32_t blk = 0; decodable && blk < blocks; blk++) {
                const size_t from = (size_t)blk * block_size;
                if (from >= data->data.size()) break;
                Bytes bd(data->data.begin() + from, data->data.begin() + std::min(data->data.size(), from + block_size));
                if ((a.flags & 8) && !a.whole_file_encrypted) xxtea_decrypt(bd.data(), bd.size(), awc_key(keys));
                std::vector<uint32_t> counts(channels);
                size_t q = 0, offsets = 0;
                for (uint32_t c = 0; c < channels; c++) {
                    if (q + 24 > bd.size()) throw Error("awc: a block too short for its channel headers");
                    std::memcpy(&counts[c], bd.data() + q + 4, 4);
                    offsets += counts[c];
                    q += 24;
                }
                q += offsets * 4;
                q = (q + 0x7FF) & ~size_t(0x7FF);
                for (uint32_t c = 0; c < channels; c++) {
                    const size_t len = (size_t)counts[c] * 2048;
                    if (q + len > bd.size()) throw Error("awc: a channel's data runs past its block");
                    chan[c].insert(chan[c].end(), bd.begin() + q, bd.begin() + q + len);
                    q += len;
                }
            }
            for (uint32_t c = 0; c < channels; c++) {
                const uint8_t* f = sf->data.data() + 12 + c * 16;
                AwcAudio s2;
                std::memcpy(&s2.id, f, 4);
                std::memcpy(&s2.samples, f + 4, 4);
                uint16_t rate;
                std::memcpy(&rate, f + 10, 2);
                s2.sample_rate = rate;
                s2.codec = f[12];
                s2.pcm = decode(chan[c], s2.codec, s2.samples);
                out.push_back(std::move(s2));
            }
            continue;
        }
        const AwcChunk* fmt = s.chunk(awc_chunk::Format);
        if (!fmt || fmt->data.size() < 20) continue;
        AwcAudio s1;
        s1.id = s.id;
        std::memcpy(&s1.samples, fmt->data.data(), 4);
        uint16_t rate;
        std::memcpy(&rate, fmt->data.data() + 8, 2);
        s1.sample_rate = rate;
        s1.codec = fmt->data[19];
        Bytes d = data->data;
        if ((a.flags & 2) && !a.whole_file_encrypted) xxtea_decrypt(d.data(), d.size(), awc_key(keys));
        s1.pcm = decode(d, s1.codec, s1.samples);
        out.push_back(std::move(s1));
    }
    return out;
}

Bytes awc_wav(const AwcAudio& s) {
    const uint32_t bytes = (uint32_t)(s.pcm.size() * 2);
    Bytes b(44 + bytes);
    auto w32 = [&](size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); };
    auto w16 = [&](size_t at, uint16_t v) { std::memcpy(b.data() + at, &v, 2); };
    std::memcpy(b.data(), "RIFF", 4);
    w32(4, 36 + bytes);
    std::memcpy(b.data() + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, 1);                    // PCM
    w16(22, 1);                    // mono
    w32(24, s.sample_rate);
    w32(28, s.sample_rate * 2);
    w16(32, 2);
    w16(34, 16);
    std::memcpy(b.data() + 36, "data", 4);
    w32(40, bytes);
    if (bytes) std::memcpy(b.data() + 44, s.pcm.data(), bytes);
    return b;
}

}  // namespace rk
