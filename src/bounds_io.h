// ragekit internal - collision bounds inside a resource.
#pragma once
#include <memory>
#include "ragekit/rsc.h"

namespace rk {

struct Bound;

/// Reads a phBound of whatever type is at ptr (null pointer gives nullptr).
std::shared_ptr<Bound> read_bound(const Reader& r, uint64_t ptr);
/// Writes a bound and its children; returns the block of the bound struct itself.
BlockId write_bound(Builder& b, const Bound& bound);

} // namespace rk
