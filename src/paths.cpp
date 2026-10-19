#include "ragekit/paths.h"
#include "ragekit/error.h"
#include "ragekit/rsc.h"

// Root (0x70 bytes), after the vtable, 1 and the page map:
//   0x10 nodes, 0x18 node count, 0x1C vehicle nodes, 0x20 ped nodes, 0x24 0
//   0x28 links, 0x30 link count, 0x34 0
//   0x38 junctions, 0x40 height map bytes, 0x48 1, 0x4C 0
//   0x50 junction refs, 0x58 count:u16, 0x5A count:u16 (the same), 0x5C 0
//   0x60 junction count, 0x64 height map byte count, 0x68 0, 0x6C 0
// The game's files lay the arrays out in that order (nodes, links, height maps, junctions, refs), each 16-aligned.

namespace rk {

namespace {

constexpr uint32_t kNodeDictionaryVft = 0x4060FA20;
constexpr int kVersion = 1;

}  // namespace

PathNodes read_ynd(std::span<const uint8_t> file) {
    const rsc::Resource res = rsc::Resource::load(file);
    if (res.version != kVersion) throw Error("ynd: resource version " + std::to_string(res.version) + ", expected 1");
    const Reader r(res);
    const uint64_t root = rsc::kSystemBase;
    PathNodes n;
    n.nodes = r.array<PathNode>(r.u64(root + 0x10), r.u32(root + 0x18));
    n.vehicle_node_count = r.u32(root + 0x1C);
    n.ped_node_count = r.u32(root + 0x20);
    n.links = r.array<PathLink>(r.u64(root + 0x28), r.u32(root + 0x30));
    n.junctions = r.array<PathJunction>(r.u64(root + 0x38), r.u32(root + 0x60));
    n.heightmaps = r.bytes(r.u64(root + 0x40), r.u32(root + 0x64));
    n.unk48 = r.u32(root + 0x48);
    n.junction_refs = r.array<PathJunctionRef>(r.u64(root + 0x50), r.u16(root + 0x58));
    n.junction_ref_count2 = r.u16(root + 0x5A);
    for (uint32_t at : {0x24u, 0x34u, 0x4Cu, 0x5Cu, 0x68u, 0x6Cu})
        if (r.u32(root + at) != 0) throw Error("ynd: the root's u32 at " + std::to_string(at) + " is not 0");
    return n;
}

Bytes write_ynd(const PathNodes& n) {
    if (n.junction_refs.size() > 0xFFFF) throw Error("ynd: more than 65535 junction references");
    if (n.nodes.size() > UINT32_MAX || n.links.size() > UINT32_MAX) throw Error("ynd: too many nodes or links");
    Builder b;
    BlockId root = b.alloc(Segment::System, 0x70);
    BlockId nodes = n.nodes.empty() ? BlockId{} : b.array(Segment::System, n.nodes);
    BlockId links = n.links.empty() ? BlockId{} : b.array(Segment::System, n.links);
    BlockId heights = n.heightmaps.empty() ? BlockId{} : b.array(Segment::System, n.heightmaps);
    BlockId junctions = n.junctions.empty() ? BlockId{} : b.array(Segment::System, n.junctions);
    BlockId refs = n.junction_refs.empty() ? BlockId{} : b.array(Segment::System, n.junction_refs);
    BlockId pages = b.alloc_pages_info();
    BlockWriter w(b, root);
    w.u32(kNodeDictionaryVft);
    w.u32(1);
    w.ptr(pages);
    w.ptr(nodes);
    w.u32((uint32_t)n.nodes.size());
    w.u32(n.vehicle_node_count);
    w.u32(n.ped_node_count);
    w.u32(0);
    w.ptr(links);
    w.u32((uint32_t)n.links.size());
    w.u32(0);
    w.ptr(junctions);
    w.ptr(heights);
    w.u32(n.unk48);
    w.u32(0);
    w.ptr(refs);
    w.u16((uint16_t)n.junction_refs.size());
    w.u16(n.junction_ref_count2);
    w.u32(0);
    w.u32((uint32_t)n.junctions.size());
    w.u32((uint32_t)n.heightmaps.size());
    return b.build(kVersion);
}

}  // namespace rk
