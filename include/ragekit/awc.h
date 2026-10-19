// ragekit - .awc audio wave containers: the game's sound effects, speech, music and radio, each file a set of streams.
//
// Layout (little endian): "ADAT", version:u16, flags:u16, stream count, the offset of the chunk data, then (flag 1) a
// u16 per stream (its first chunk's index), a u32 per stream (id in the low 29 bits, chunk count in the top 3), an 8-byte
// info per chunk (type << 56 | size << 28 | offset), and the chunks: format (codec, sample rate, samples), data, seek
// table, markers, peak, stream format (the channels of a multi-channel stream) ...
//
// Encryption, with the XXTEA key of RpfKeys::awc: a whole file (flag 0xFF00 files whose magic does not read), the data
// chunk of a single-channel file (flag 2) or each block of a multi-channel file's data (flag 8). ragekit keeps the
// chunks as stored, so a file is written back byte for byte; awc_decode decrypts and decodes the audio.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "rpf.h"
#include "types.h"

namespace rk {

/// The chunk types (the low byte of the joaat of their names).
namespace awc_chunk {
constexpr uint8_t Data = 0x55, Format = 0xFA, Animation = 0x5C, Peak = 0x36, Mid = 0x68, Gesture = 0x2B, GranularGrains = 0x5A,
                  GranularLoops = 0xD9, Markers = 0xBD, StreamFormat = 0x48, SeekTable = 0xA3;
}

struct AwcChunk {
    uint8_t type = 0;
    uint32_t offset = 0;   // in the file (as read; write_awc places the chunk there, pack() lays them out anew)
    Bytes data;            // as stored (still encrypted where the flags say)
};

struct AwcStream {
    uint32_t id = 0;       // joaat of the stream's name, 29 bits
    std::vector<AwcChunk> chunks;
    const AwcChunk* chunk(uint8_t type) const;
};

struct AwcFile {
    uint16_t version = 1;
    uint16_t flags = 0xFF00;          // 1 chunk indices, 2 single-channel data encrypted, 4 multi-channel, 8 multi-channel blocks encrypted
    std::vector<uint16_t> chunk_indices;   // per stream with flag 1
    std::vector<AwcStream> streams;
    bool whole_file_encrypted = false;
    uint32_t size = 0;                // the file's size as read (write_awc keeps trailing bytes up to it)

    bool multi_channel() const { return flags & 4; }
    /// Lays the chunks out anew after an edit, as the game's files are: the table, then format / peak / stream format
    /// chunks, seek tables, markers and other small chunks, the data last, each in stream order; data aligned to 16 bytes,
    /// markers and granular chunks to 4.
    void pack();
};

/// An ADAT file, or one that may be a whole encrypted one (its size a multiple of 4). Enhanced's
/// shaders/fxdb/sga_win32_60_final*.awc are shader caches, not audio, and are not.
bool is_awc(std::span<const uint8_t> file);
/// Throws rk::Error for a malformed file, or a whole-file encrypted one when `keys` (default RpfKeys::default_keys()) has
/// no AWC key.
AwcFile read_awc(std::span<const uint8_t> file, std::shared_ptr<const RpfKeys> keys = {});
Bytes write_awc(const AwcFile& a, std::shared_ptr<const RpfKeys> keys = {});

/// One playable sound: a single-channel stream, or one channel of a multi-channel stream.
struct AwcAudio {
    uint32_t id = 0;
    uint32_t sample_rate = 0;
    uint32_t samples = 0;
    uint8_t codec = 0;                // 0 PCM, 4 IMA ADPCM (as stored); decoded to 16-bit PCM either way
    std::vector<int16_t> pcm;         // empty for another codec
    bool decoded() const { return codec == 0 || codec == 4; }
};

/// Every sound of the file, decrypted and decoded (PCM and ADPCM; sounds of other codecs come without samples).
std::vector<AwcAudio> awc_decode(const AwcFile& a, std::shared_ptr<const RpfKeys> keys = {});
/// A 16-bit mono .wav of a sound.
Bytes awc_wav(const AwcAudio& s);

}  // namespace rk
