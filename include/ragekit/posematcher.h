// ragekit - pose matchers (.ypdb in binary form, the byte 0x1A first): for each sample a clip and a time in it, and the
// point cloud of bone positions the game compares a ped's pose against to pick where a clip starts.
#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include "types.h"

namespace rk {

struct PoseMatcherPointCloud {
    std::vector<Vec3> points;
    std::vector<float> weights;   // one per point
    Vec3 bounds_min, bounds_max;
    float weights_sum = 0;
};

struct PoseMatcherSample {
    uint32_t clip_set = 0, clip = 0;   // name hashes
    float offset = 0;                  // the time in the clip
    PoseMatcherPointCloud cloud;
};

struct PoseMatcher {
    int32_t serializer_version = 2, pose_matcher_version = 0;
    uint32_t signature = 0;            // written twice: after the versions and at the end
    std::vector<PoseMatcherSample> samples;
    std::vector<uint16_t> bone_tags;
    std::vector<float> weights;        // the weight set, one per bone tag
    float unk7 = 0.033333f;            // 1/30 in every game file (CodeWalker "Unk7"; GUESS the sample interval in seconds)
    int32_t unk8 = 1;                  // 1 in every game file
    uint32_t signature2 = 0;           // the closing copy of the signature
};

bool is_pose_matcher(std::span<const uint8_t> file);
PoseMatcher read_ypdb(std::span<const uint8_t> file);
Bytes write_ypdb(const PoseMatcher& p);

}  // namespace rk
