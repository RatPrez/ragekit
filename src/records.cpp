#include "ragekit/records.h"
#include <algorithm>
#include "ragekit/error.h"
#include "ragekit/rsc.h"

// Vehicle recording (.yvr), root 0x20 bytes:
//   0x00 vft, 0x04 1, 0x08 page map, 0x10 entries pointer, 0x18 count:u16, 0x1A capacity:u16, 0x1C 0
// Waypoint recording (.ywr), root 0x30 bytes:
//   0x00 vft, 0x04 1, 0x08 page map, 0x10 0, 0x18 entries pointer, 0x20 count:u32, 0x24 .. 0x2F 0

namespace rk {

namespace {

constexpr uint32_t kVehicleRecordVft = 0x40573228;
constexpr uint32_t kWaypointRecordVft = 0x40620EA8;
constexpr int kVersion = 1;

rsc::Resource load(std::span<const uint8_t> file, const char* what) {
    rsc::Resource res = rsc::Resource::load(file);
    if (res.version != kVersion) throw Error(std::string(what) + ": resource version " + std::to_string(res.version) + ", expected 1");
    return res;
}

}  // namespace

VehicleRecording read_yvr(std::span<const uint8_t> file) {
    const rsc::Resource res = load(file, "yvr");
    const Reader r(res);
    const uint64_t root = rsc::kSystemBase;
    VehicleRecording v;
    v.entries = r.array<VehicleRecordEntry>(r.u64(root + 0x10), r.u16(root + 0x18));
    v.capacity = r.u16(root + 0x1A);
    return v;
}

Bytes write_yvr(const VehicleRecording& v) {
    if (v.entries.size() > 0xFFFF) throw Error("yvr: more than 65535 entries");
    Builder b;
    BlockId root = b.alloc(Segment::System, 0x20);
    BlockId entries = v.entries.empty() ? BlockId{} : b.array(Segment::System, v.entries);
    BlockId pages = b.alloc_pages_info();
    BlockWriter w(b, root);
    w.u32(kVehicleRecordVft);
    w.u32(1);
    w.ptr(pages);
    w.ptr(entries);
    w.u16((uint16_t)v.entries.size());
    w.u16(std::max<uint16_t>(v.capacity, (uint16_t)v.entries.size()));
    return b.build(kVersion);
}

WaypointRecording read_ywr(std::span<const uint8_t> file) {
    const rsc::Resource res = load(file, "ywr");
    const Reader r(res);
    const uint64_t root = rsc::kSystemBase;
    WaypointRecording w;
    w.entries = r.array<WaypointRecordEntry>(r.u64(root + 0x18), r.u32(root + 0x20));
    return w;
}

Bytes write_ywr(const WaypointRecording& wr) {
    if (wr.entries.size() > UINT32_MAX) throw Error("ywr: too many entries");
    Builder b;
    BlockId root = b.alloc(Segment::System, 0x30);
    BlockId entries = wr.entries.empty() ? BlockId{} : b.array(Segment::System, wr.entries);
    BlockId pages = b.alloc_pages_info();
    BlockWriter w(b, root);
    w.u32(kWaypointRecordVft);
    w.u32(1);
    w.ptr(pages);
    w.u64(0);
    w.ptr(entries);
    w.u32((uint32_t)wr.entries.size());
    return b.build(kVersion);
}

}  // namespace rk
