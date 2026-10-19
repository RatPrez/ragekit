#include "ragekit/posematcher.h"
#include <cstring>
#include "ragekit/error.h"

namespace rk {

namespace {

struct In {
    std::span<const uint8_t> b;
    size_t p = 1;
    template <class T> T get() {
        if (p + sizeof(T) > b.size()) throw Error("ypdb: the file ends at byte " + std::to_string(p));
        T v;
        std::memcpy(&v, b.data() + p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    size_t count(size_t item) {
        const int32_t n = get<int32_t>();
        if (n < 0 || (size_t)n > (b.size() - p) / item) throw Error("ypdb: a count of " + std::to_string(n) + " at byte " + std::to_string(p - 4) + " does not fit the file");
        return (size_t)n;
    }
    Vec3 vec3() { Vec3 v; v.x = get<float>(); v.y = get<float>(); v.z = get<float>(); return v; }
};

struct Out {
    Bytes b{0x1A};
    template <class T> void put(T v) { b.insert(b.end(), (const uint8_t*)&v, (const uint8_t*)&v + sizeof(T)); }
    void vec3(const Vec3& v) { put(v.x); put(v.y); put(v.z); }
};

}  // namespace

bool is_pose_matcher(std::span<const uint8_t> file) { return file.size() >= 13 && file[0] == 0x1A; }

PoseMatcher read_ypdb(std::span<const uint8_t> file) {
    if (!is_pose_matcher(file)) throw Error("ypdb: not a binary pose matcher (no 0x1A first)");
    In in{file};
    PoseMatcher m;
    m.serializer_version = in.get<int32_t>();
    m.pose_matcher_version = in.get<int32_t>();
    m.signature = in.get<uint32_t>();
    const size_t ns = in.count(12 + 8 + 28);
    for (size_t i = 0; i < ns; i++) {
        PoseMatcherSample s;
        s.clip_set = in.get<uint32_t>();
        s.clip = in.get<uint32_t>();
        s.offset = in.get<float>();
        const size_t np = in.count(12);
        for (size_t k = 0; k < np; k++) s.cloud.points.push_back(in.vec3());
        const size_t nw = in.count(4);
        for (size_t k = 0; k < nw; k++) s.cloud.weights.push_back(in.get<float>());
        s.cloud.bounds_min = in.vec3();
        s.cloud.bounds_max = in.vec3();
        s.cloud.weights_sum = in.get<float>();
        m.samples.push_back(std::move(s));
    }
    const size_t nb = in.count(2);
    for (size_t i = 0; i < nb; i++) m.bone_tags.push_back(in.get<uint16_t>());
    const size_t nw = in.count(4);
    for (size_t i = 0; i < nw; i++) m.weights.push_back(in.get<float>());
    m.unk7 = in.get<float>();
    m.unk8 = in.get<int32_t>();
    m.signature2 = in.get<uint32_t>();
    if (in.p != file.size()) throw Error("ypdb: " + std::to_string(file.size() - in.p) + " bytes after the pose matcher");
    return m;
}

Bytes write_ypdb(const PoseMatcher& m) {
    Out o;
    o.put(m.serializer_version);
    o.put(m.pose_matcher_version);
    o.put(m.signature);
    o.put((int32_t)m.samples.size());
    for (const PoseMatcherSample& s : m.samples) {
        o.put(s.clip_set);
        o.put(s.clip);
        o.put(s.offset);
        o.put((int32_t)s.cloud.points.size());
        for (const Vec3& v : s.cloud.points) o.vec3(v);
        o.put((int32_t)s.cloud.weights.size());
        for (float w : s.cloud.weights) o.put(w);
        o.vec3(s.cloud.bounds_min);
        o.vec3(s.cloud.bounds_max);
        o.put(s.cloud.weights_sum);
    }
    o.put((int32_t)m.bone_tags.size());
    for (uint16_t t : m.bone_tags) o.put(t);
    o.put((int32_t)m.weights.size());
    for (float w : m.weights) o.put(w);
    o.put(m.unk7);
    o.put(m.unk8);
    o.put(m.signature2);
    return o.b;
}

}  // namespace rk
