// ragekit internal: a table-driven raw-deflate decoder for whole, well-formed streams.
#pragma once
#include <cstddef>
#include <cstdint>

namespace rk::detail {

/// Inflates a raw deflate stream into out[0, out_cap). True only when the stream is well formed and its final block ends
/// within the input and the buffer: out_len is then the stream's length and out holds exactly what any inflater gives.
/// False for anything else (a corrupt or truncated stream, more output than out_cap, a code this decoder refuses): the
/// caller then inflates with miniz, which decides what such a stream gives. Nothing past out_len is promised on false.
bool inflate_fast(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap, size_t& out_len);

}  // namespace rk::detail
