// ragekit - recordings: vehicle recordings (.yvr, the "carrec" paths scripted cars drive) and waypoint recordings
// (.ywr, the routes scripted peds and vehicles follow).
//
// Both are a small resource: the root structure (a page map and one list) and an array of fixed-size entries.
#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include "game.h"
#include "types.h"

namespace rk {

/// One sample of a vehicle recording (32 bytes, as stored).
struct VehicleRecordEntry {
    uint32_t time = 0;               // milliseconds from the start
    int16_t velocity[3] = {};        // m/s x 273.0583 (32767 / 120)
    int8_t right[3] = {};            // the vehicle's right vector x 127
    int8_t forward[3] = {};          // its forward vector x 127
    int8_t steering = 0;             // steering angle x 20 (radians)
    int8_t gas = 0;                  // -100 .. 100, negative = reverse
    int8_t brake = 0;                // 0 .. 100
    uint8_t handbrake = 0;           // 0 or 1
    Vec3 position;

    Vec3 velocity_ms() const { return {velocity[0] / 273.0583f, velocity[1] / 273.0583f, velocity[2] / 273.0583f}; }
    Vec3 right_vector() const { return {right[0] / 127.f, right[1] / 127.f, right[2] / 127.f}; }
    Vec3 forward_vector() const { return {forward[0] / 127.f, forward[1] / 127.f, forward[2] / 127.f}; }
};
static_assert(sizeof(VehicleRecordEntry) == 32);

struct VehicleRecording {
    std::vector<VehicleRecordEntry> entries;
    uint16_t capacity = 0;           // the list's capacity as read (written as at least the entry count)
};

/// One point of a waypoint route (20 bytes, as stored). docs/other-formats.md has the statistics behind the names.
struct WaypointRecordEntry {
    Vec3 position;
    uint16_t flags = 0;              // bit flags: 0x80 on 55 % of the points, 0x01 0x02 0x20 0x40 rare
    uint8_t heading = 0;             // the direction to the next point, 256ths of a turn from +y counter-clockwise
    uint8_t speed = 0;               // 64 / 128 / 192 on pedestrian routes (2 m apart), larger on vehicle routes
    uint16_t unk2 = 0;               // 0 on 84 %, else often two equal bytes (0x5F5F, 0x4F4F ...)
    uint16_t unk3 = 0;               // 0 on all but 12 of 106,096 points

    float heading_radians() const { return heading * 6.28318531f / 256.f; }
};
static_assert(sizeof(WaypointRecordEntry) == 20);

struct WaypointRecording {
    std::vector<WaypointRecordEntry> entries;
};

VehicleRecording read_yvr(std::span<const uint8_t> file);
Bytes write_yvr(const VehicleRecording& r);
WaypointRecording read_ywr(std::span<const uint8_t> file);
Bytes write_ywr(const WaypointRecording& r);

}  // namespace rk
