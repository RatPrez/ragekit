// Internal: SHA1, AES-256 (ECB) and the game's "NG" block cipher, for RPF archives.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace rk::rpfcrypto {

/// SHA1 of a buffer (20 bytes out).
void sha1(const uint8_t* data, size_t len, uint8_t out[20]);

/// AES-256 ECB, no padding; a trailing partial block is left as is. In place.
void aes_decrypt(uint8_t* data, size_t len, const uint8_t key[32]);
void aes_encrypt(uint8_t* data, size_t len, const uint8_t key[32]);

/// The game's file name hash (keys the NG key choice): uses the 256 byte lookup table.
uint32_t name_hash(std::string_view name, const uint8_t* lut);

/// NG decrypt in place. `keys` = 101 x 272 bytes, `tables` = 17 x 16 x 256 words. The key is
/// picked by the name hash and `length` (a file's size field). A trailing partial block is left as is.
void ng_decrypt(uint8_t* data, size_t len, std::string_view name, uint32_t length, const uint8_t* keys,
                const uint32_t* tables, const uint8_t* lut);

} // namespace rk::rpfcrypto
