// Consistent-hash ring mapping node keys to shards, carried over from
// cdc-store's ConsistentHashRing (same mix64 and vnode placement).
#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>

namespace gv {

inline uint64_t mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

class HashRing {
public:
    HashRing(int shards, int vnodes_per_shard) : shards_(shards) {
        if (shards < 1) throw std::invalid_argument("need at least one shard");
        for (int s = 0; s < shards; ++s)
            for (int v = 0; v < vnodes_per_shard; ++v)
                ring_[mix64((uint64_t(s) << 32) ^ uint64_t(v) * 0x100000001b3ULL)] = s;
    }
    int shards() const { return shards_; }
    // First ring point clockwise from the key's hash.
    int shard_of(uint64_t key) const {
        auto it = ring_.lower_bound(mix64(key));
        return it == ring_.end() ? ring_.begin()->second : it->second;
    }

private:
    int shards_;
    std::map<uint64_t, int> ring_;
};

} // namespace gv
