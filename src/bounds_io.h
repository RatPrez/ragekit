// ragekit internal - collision bounds inside a resource.
#pragma once
#include <map>
#include <memory>
#include "ragekit/rsc.h"

namespace rk {

struct Bound;

/// Reads a phBound of whatever type is at ptr (null pointer gives nullptr). With `seen`, a bound already read at the same
/// address (a composite's child included) is the same object: a fragment's drawable points at a child of its archetype's
/// composite (the glass panes' Box), and a written file must keep one block for it.
std::shared_ptr<Bound> read_bound(const Reader& r, uint64_t ptr, std::map<uint64_t, std::shared_ptr<Bound>>* seen = nullptr);
/// Writes a bound and its children; returns the block of the bound struct itself. With `written`, a bound (a composite's
/// child included) already written is pointed at again instead of being written twice: the game constructs each bound once,
/// so a second copy keeps the file's vtable word and its first virtual call crashes.
BlockId write_bound(Builder& b, const Bound& bound, std::map<const Bound*, BlockId>* written = nullptr);

} // namespace rk
