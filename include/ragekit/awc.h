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

/// A single-channel stream's format chunk: 20 bytes, or 24 with the peak of the first 4,096 samples.
struct AwcFormat {
    uint32_t samples = 0;
    int32_t  loop_point = -1;         // the sample a looping sound goes back to; -1 = plays once
    uint16_t sample_rate = 0;
    int16_t  headroom = 0;
    uint16_t loop_begin = 0, loop_end = 0, play_end = 0;
    uint8_t  play_begin = 0;
    uint8_t  codec = 0;               // 0 PCM, 4 IMA ADPCM
    bool     has_peak = false;
    uint32_t peak = 0;                // low 16 bits: min(2 |sample|, 65535) over the first 4,096 samples
    /// Throws rk::Error for a chunk shorter than 20 bytes.
    static AwcFormat read(std::span<const uint8_t> chunk);
    Bytes bytes() const;
};

/// One sound to put into a wave pack: 16-bit mono samples.
struct AwcSoundSource {
    std::string name;                 // the stream id is joaat_lower(name) & 0x1FFFFFFF, what a SimpleSound's wave field names
    uint32_t sample_rate = 0;         // at most 65,535 (the format chunk's field is 16 bits)
    std::vector<int16_t> pcm;
    uint8_t codec = 0;                // 0 PCM (the game's sound effect packs), 4 IMA ADPCM (its speech and one-shot ambience)
    int32_t loop_point = -1;          // 0 = the whole sound loops
};

/// A wave pack laid out as the game's single-channel packs are (docs/pso-and-audio.md): flags 0xFF01 (chunk indices,
/// nothing encrypted), streams in ascending id order, each a data chunk, a format chunk with the peak and a peak chunk
/// when the sound is 8,192 samples or longer (PCM: peak, data, format; ADPCM: data, format, peak). Throws rk::Error for
/// no sounds, an empty sound, a rate past 65,535, two names of one id, a loop point past the end or another codec.
AwcFile make_awc(const std::vector<AwcSoundSource>& sounds);
/// IMA ADPCM as the game stores it (blocks of 2,048 bytes: the step index, 0, the predictor as an i16, then 4,088
/// samples as nibbles, low nibble first); awc_decode reads it back.
Bytes awc_encode_adpcm(std::span<const int16_t> pcm);
/// The peak values of a sound: [0] over the first 4,096 samples (the format chunk's), then one per further whole block of
/// 4,096 that starts before the last 4,096 (the peak chunk's).
std::vector<uint16_t> awc_peaks(std::span<const int16_t> pcm);

/// A .wav file's audio: 16-bit PCM (format 1, or WAVE_FORMAT_EXTENSIBLE with the PCM subformat), its channels
/// interleaved. Throws rk::Error with a plain message for anything else (another bit depth, a compressed format, no data).
struct WavAudio {
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    std::vector<int16_t> samples;     // interleaved
    size_t frames() const { return channels ? samples.size() / channels : 0; }
    /// The channels averaged into one.
    std::vector<int16_t> mono() const;
};
WavAudio read_wav(std::span<const uint8_t> file);

}  // namespace rk
