#include "ragekit/clip.h"
#include <set>
#include "ragekit/error.h"
#include "graph_io.h"

// The structures (offsets in bytes; CodeWalker's Clip.cs names them):
//   crClipDictionary 0x40     0x08 page map, 0x18 animation map, 0x28 clip buckets (pointer array), 0x30 bucket count:u16, 0x32 clips:u16
//   crAnimationMap 0x30       0x18 buckets (pointer array of entries), 0x20 bucket count:u16, 0x22 animations:u16
//   animation entry 0x20      0x00 hash, 0x08 crAnimation, 0x10 next entry of the bucket
//   crAnimation 0x60          0x10 flags:u8, 0x14 frames:u16, 0x16 sequence frame limit:u16, 0x18 duration, 0x1C hash, 0x38 the
//                             longest sequence, 0x3C usage count, 0x40 sequences (pointer list: ptr, count:u16, capacity:u16),
//                             0x50 bone ids (ptr, count:u16, capacity:u16; 4 bytes each)
//   sequence 0x20 + n         0x04 n (the data's length), then the header's frame and channel counts; the data follows
//   clip entry 0x20           0x00 hash, 0x08 crClip, 0x10 next entry
//   crClip 0x70               0x10 type (1 animation, 2 animation list), 0x18 name, 0x20 length:u16, 0x28 the dictionary,
//                             0x38 tag list, 0x40 property map; type 1: 0x50 crAnimation, 0x58 start, 0x5C end, 0x60 rate;
//                             type 2: 0x50 entries (24 bytes: start, end, rate, 0, 0x10 crAnimation), 0x58 count:u16, 0x60 duration
//   property map 0x10         0x00 buckets (pointer array of entries), 0x08 bucket count:u16, 0x0A properties:u16
//   property entry 0x20       0x00 hash, 0x08 crProperty, 0x10 next entry
//   crProperty 0x40           0x18 name hash, 0x20 attributes (pointer array), 0x28 count:u16, 0x2A capacity:u16
//   crPropertyAttribute 0x30  0x08 type (1 float, 2 int, 3 bool, 4 string, 6 vector3, 8 vector4, 12 hash string), 0x18 name hash;
//                             a string's text at 0x20
//   crTagList 0x20            0x00 tags (pointer array), 0x08 count:u16, 0x0A count:u16
//   crTag 0x50                a crProperty, then 0x40 start phase, 0x44 end phase, 0x48 its tag list

namespace rk {

namespace {

constexpr uint16_t K(ClipKind k) { return (uint16_t)k; }

class ClipReader {
public:
    explicit ClipReader(const Reader& r) : g_(r, "ycd"), r_(r) {}
    GraphReader g_;

    RscBlockPtr dictionary() {
        return g_.block(rsc::kSystemBase, K(ClipKind::Dictionary), 0x40, [&](RscBlock& b, uint64_t a) {
            g_.take(b, 0x08);   // the page map: written anew
            g_.own(b, 0x18, [&](uint64_t p) { return animation_map(p); });
            const uint16_t buckets = r_.u16(a + 0x30);
            g_.own(b, 0x28, [&](uint64_t p) { return pointer_array(p, buckets, [&](uint64_t q) { return clip_entry(q); }); });
        });
    }

private:
    const Reader& r_;

    RscBlockPtr pointer_array(uint64_t p, size_t n, const std::function<RscBlockPtr(uint64_t)>& item) {
        if (!n) return nullptr;
        return g_.block(p, K(ClipKind::PointerArray), n * 8, [&](RscBlock& b, uint64_t) {
            for (size_t i = 0; i < n; i++) g_.own(b, (uint32_t)(i * 8), item);
        });
    }

    RscBlockPtr animation_map(uint64_t p) {
        return g_.block(p, K(ClipKind::AnimationMap), 0x30, [&](RscBlock& b, uint64_t a) {
            const uint16_t buckets = r_.u16(a + 0x20);
            g_.own(b, 0x18, [&](uint64_t q) { return pointer_array(q, buckets, [&](uint64_t e) { return animation_entry(e); }); });
        });
    }

    RscBlockPtr animation_entry(uint64_t p) {
        return g_.block(p, K(ClipKind::AnimationEntry), 0x20, [&](RscBlock& b, uint64_t) {
            g_.data(b, 0x00);
            g_.own(b, 0x08, [&](uint64_t q) { return animation(q); });
            g_.own(b, 0x10, [&](uint64_t q) { return animation_entry(q); });
        });
    }

    RscBlockPtr animation(uint64_t p) {
        return g_.block(p, K(ClipKind::Animation), 0x60, [&](RscBlock& b, uint64_t a) {
            const size_t seqs = std::max(r_.u16(a + 0x48), r_.u16(a + 0x4A));
            g_.own(b, 0x40, [&](uint64_t q) { return pointer_array(q, seqs, [&](uint64_t s) { return sequence(s); }); });
            const size_t ids = std::max(r_.u16(a + 0x58), r_.u16(a + 0x5A));
            g_.own(b, 0x50, [&](uint64_t q) { return ids ? g_.block(q, K(ClipKind::BoneIds), ids * 4) : nullptr; });
        });
    }

    RscBlockPtr sequence(uint64_t p) { return g_.block(p, K(ClipKind::Sequence), 0x20 + (size_t)r_.u32(p + 4)); }

    RscBlockPtr clip_entry(uint64_t p) {
        return g_.block(p, K(ClipKind::ClipEntry), 0x20, [&](RscBlock& b, uint64_t) {
            g_.data(b, 0x00);
            g_.own(b, 0x08, [&](uint64_t q) { return clip(q); });
            g_.own(b, 0x10, [&](uint64_t q) { return clip_entry(q); });
        });
    }

    RscBlockPtr clip(uint64_t p) {
        return g_.block(p, K(ClipKind::Clip), 0x70, [&](RscBlock& b, uint64_t a) {
            const uint32_t type = r_.u32(a + 0x10);
            g_.string(b, 0x18, K(ClipKind::String));
            g_.weak(b, 0x28);
            g_.own(b, 0x38, [&](uint64_t q) { return tag_list(q); });
            g_.own(b, 0x40, [&](uint64_t q) { return property_map(q); });
            if (type == (uint32_t)ClipType::Animation) g_.own(b, 0x50, [&](uint64_t q) { return animation(q); });
            else if (type == (uint32_t)ClipType::AnimationList) {
                const size_t n = r_.u16(a + 0x58);
                g_.own(b, 0x50, [&](uint64_t q) {
                    return n ? g_.block(q, K(ClipKind::ClipAnimations), n * 24, [&](RscBlock& l, uint64_t) {
                        for (size_t i = 0; i < n; i++) g_.own(l, (uint32_t)(i * 24 + 0x10), [&](uint64_t s) { return animation(s); });
                    }) : nullptr;
                });
            } else throw Error("ycd: clip type " + std::to_string(type) + " is not known");
        });
    }

    RscBlockPtr property_map(uint64_t p) {
        return g_.block(p, K(ClipKind::PropertyMap), 0x10, [&](RscBlock& b, uint64_t a) {
            const uint16_t buckets = r_.u16(a + 0x08);
            g_.own(b, 0x00, [&](uint64_t q) { return pointer_array(q, buckets, [&](uint64_t e) { return property_entry(e); }); });
        });
    }

    RscBlockPtr property_entry(uint64_t p) {
        return g_.block(p, K(ClipKind::PropertyEntry), 0x20, [&](RscBlock& b, uint64_t) {
            g_.data(b, 0x00);
            g_.own(b, 0x08, [&](uint64_t q) { return property(q, K(ClipKind::Property), 0x40); });
            g_.own(b, 0x10, [&](uint64_t q) { return property_entry(q); });
        });
    }

    // a crProperty, or a crTag (a crProperty with phases and its tag list)
    RscBlockPtr property(uint64_t p, uint16_t kind, size_t size) {
        return g_.block(p, kind, size, [&](RscBlock& b, uint64_t a) {
            g_.data(b, 0x18);   // name hash
            g_.data(b, 0x38);   // a second hash
            const size_t n = std::max(r_.u16(a + 0x28), r_.u16(a + 0x2A));
            g_.own(b, 0x20, [&](uint64_t q) { return pointer_array(q, n, [&](uint64_t s) { return attribute(s); }); });
            if (kind == K(ClipKind::Tag)) g_.own(b, 0x48, [&](uint64_t q) { return tag_list(q); });
        });
    }

    RscBlockPtr attribute(uint64_t p) {
        return g_.block(p, K(ClipKind::Attribute), 0x30, [&](RscBlock& b, uint64_t a) {
            const uint8_t type = r_.u8(a + 0x08);
            g_.data(b, 0x18);                  // name hash
            if (type != 4) g_.data(b, 0x20);   // the value: an int, a hash ...
            switch (type) {
                case 1: case 2: case 3: case 6: case 8: case 12: break;
                case 4: g_.string(b, 0x20, K(ClipKind::String)); break;
                default: throw Error("ycd: property attribute type " + std::to_string(type) + " is not known");
            }
        });
    }

    RscBlockPtr tag_list(uint64_t p) {
        return g_.block(p, K(ClipKind::TagList), 0x20, [&](RscBlock& b, uint64_t a) {
            const size_t n = std::max(r_.u16(a + 0x08), r_.u16(a + 0x0A));
            g_.own(b, 0x00, [&](uint64_t q) { return pointer_array(q, n, [&](uint64_t s) { return property(s, K(ClipKind::Tag), 0x50); }); });
        });
    }
};

/// The entries of a hash map: every bucket's chain, in bucket order.
std::vector<RscBlockPtr> chain_entries(const RscBlockPtr& buckets) {
    std::vector<RscBlockPtr> out;
    if (!buckets) return out;
    std::set<const RscBlock*> seen;   // a chain that loops back (a damaged file) ends where it repeats
    for (const RscBlock::Link& l : buckets->links)
        for (RscBlockPtr e = l.to; e && seen.insert(e.get()).second; e = e->target(0x10)) out.push_back(e);
    return out;
}

}  // namespace

std::vector<AnimationBoneId> AnimationRef::bone_ids() const {
    std::vector<AnimationBoneId> v;
    RscBlockPtr ids = block ? block->target(0x50) : nullptr;
    if (!ids) return v;
    v.resize(std::min<size_t>(block->get<uint16_t>(0x58), ids->bytes.size() / 4));
    if (!v.empty()) std::memcpy(v.data(), ids->bytes.data(), v.size() * 4);
    return v;
}

std::vector<RscBlockPtr> AnimationRef::sequences() const {
    std::vector<RscBlockPtr> v;
    RscBlockPtr arr = block ? block->target(0x40) : nullptr;
    if (!arr) return v;
    const size_t n = block->get<uint16_t>(0x48);
    for (const RscBlock::Link& l : arr->links) if (l.at / 8 < n) v.push_back(l.to);
    return v;
}

std::vector<RscBlockPtr> ClipRef::animations() const {
    std::vector<RscBlockPtr> v;
    if (!block) return v;
    if (type() == ClipType::Animation) { if (auto a = block->target(0x50)) v.push_back(a); }
    else if (auto list = block->target(0x50))
        for (const RscBlock::Link& l : list->links) v.push_back(l.to);
    return v;
}

std::vector<uint32_t> ClipRef::property_names() const {
    std::vector<uint32_t> v;
    RscBlockPtr map = block ? block->target(0x40) : nullptr;
    if (!map) return v;
    for (const RscBlockPtr& e : chain_entries(map->target(0x00))) v.push_back(e->get<uint32_t>(0));
    return v;
}

size_t ClipRef::tag_count() const {
    RscBlockPtr tags = block ? block->target(0x38) : nullptr;
    return tags ? tags->get<uint16_t>(0x08) : 0;
}

std::vector<AnimationRef> ClipDictionary::animations() const {
    std::vector<AnimationRef> v;
    RscBlockPtr map = root ? root->target(0x18) : nullptr;
    if (!map) return v;
    for (const RscBlockPtr& e : chain_entries(map->target(0x18)))
        if (RscBlockPtr a = e->target(0x08)) v.push_back({e->get<uint32_t>(0), a});   // an entry without an animation is skipped
    return v;
}

std::vector<ClipRef> ClipDictionary::clips() const {
    std::vector<ClipRef> v;
    if (!root) return v;
    for (const RscBlockPtr& e : chain_entries(root->target(0x28)))
        if (RscBlockPtr c = e->target(0x08)) v.push_back({e->get<uint32_t>(0), c});   // an entry without a clip is skipped
    return v;
}

AnimationRef ClipDictionary::animation(uint32_t hash) const {
    for (const AnimationRef& a : animations()) if (a.hash == hash) return a;
    return {};
}

ClipRef ClipDictionary::clip(uint32_t hash) const {
    for (const ClipRef& c : clips()) if (c.hash == hash) return c;
    return {};
}

ClipDictionary read_ycd_checked(std::span<const uint8_t> file, size_t* census, std::string* where) {
    const rsc::Resource res = rsc::Resource::load(file);
    if (res.version != 46) throw Error("ycd: resource version " + std::to_string(res.version) + ", expected 46");
    const Reader r(res);
    ClipReader cr(r);
    ClipDictionary d;
    d.root = cr.dictionary();
    cr.g_.resolve();
    if (census) *census = cr.g_.census(where, {K(ClipKind::Sequence), K(ClipKind::BoneIds), K(ClipKind::String)});
    return d;
}

ClipDictionary read_ycd(std::span<const uint8_t> file) { return read_ycd_checked(file, nullptr); }

Bytes write_ycd(const ClipDictionary& d) {
    Builder b;
    write_graph(b, d.root);
    return b.build(46);
}

}  // namespace rk
