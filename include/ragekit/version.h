// ragekit - the library's version (kept equal to project(... VERSION) in CMakeLists.txt and to the release tag vX.Y.Z).
#pragma once

#define RAGEKIT_VERSION_MAJOR 0
#define RAGEKIT_VERSION_MINOR 2
#define RAGEKIT_VERSION_PATCH 0
#define RAGEKIT_VERSION_STRING "0.2.0"

namespace rk {
constexpr int kVersionMajor = RAGEKIT_VERSION_MAJOR, kVersionMinor = RAGEKIT_VERSION_MINOR, kVersionPatch = RAGEKIT_VERSION_PATCH;
constexpr const char* kVersion = RAGEKIT_VERSION_STRING;
} // namespace rk
