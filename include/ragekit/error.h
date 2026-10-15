#pragma once
#include <stdexcept>
#include <string>

namespace rk {

/// Thrown for malformed input or unsupported data. Message says what and where.
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what) : std::runtime_error(what) {}
};

} // namespace rk
