// Filtered top-k strategies over an Hnsw index, and the VectorHook that
// plugs them into Cypher's ORDER BY vec_dist(n, $q) LIMIT k.
//
//   prefilter  : exact distances over just the matching ids (brute force).
//                Recall 1.0; cost grows with the number of matches.
//   postfilter : ordinary HNSW search for ef candidates, then drop the ones
//                that fail the predicate. Cheap, but when few nodes match,
//                few (or none) of the ef candidates survive.
//   inline     : HNSW traversal that only admits matching nodes into the
//                result set (hnswlib's filter functor). Keeps recall up, but
//                walks more of the graph as the filter gets more selective.
//   adaptive   : prefilter below a match-count threshold, inline above it.
#pragma once

#include "graphvec/cypher.hpp"
#include "graphvec/hnsw.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace gv {

using Hits = std::vector<Hnsw::Hit>;

inline Hits prefilter_search(const Hnsw& h, const float* q, size_t k, const std::vector<uint32_t>& ids) {
    Hits all;
    all.reserve(ids.size());
    for (uint32_t i : ids) all.push_back({h.dist(i, q), i});
    size_t kk = std::min(k, all.size());
    std::partial_sort(all.begin(), all.begin() + long(kk), all.end());
    all.resize(kk);
    return all;
}

inline Hits postfilter_search(const Hnsw& h, const float* q, size_t k, size_t ef,
                              const std::vector<uint8_t>& allow) {
    Hits cand = h.search(q, ef, ef);
    Hits out;
    for (auto& c : cand)
        if (allow[c.second] && out.size() < k) out.push_back(c);
    return out;
}

inline Hits inline_search(const Hnsw& h, const float* q, size_t k, size_t ef, const std::vector<uint8_t>& allow) {
    return h.search_filtered(q, k, ef, [&](uint32_t id) { return allow[id] != 0; });
}

inline VectorHook make_vector_hook(const Hnsw& h, size_t ef, size_t brute_threshold) {
    VectorHook v;
    v.dist = [&h](NodeId n, const std::vector<float>& q) { return h.dist(n, q.data()); };
    v.search = [&h, ef, brute_threshold](const std::vector<float>& q, size_t k, const std::vector<NodeId>* m,
                                         std::string& strategy) {
        std::vector<std::pair<float, NodeId>> out;
        Hits hits;
        if (!m) {
            strategy = "hnsw";
            hits = h.search(q.data(), k, std::max(ef, k));
        } else if (m->size() <= brute_threshold) {
            strategy = "prefilter";
            hits = prefilter_search(h, q.data(), k, *m);
        } else {
            strategy = "inline";
            std::vector<uint8_t> allow(h.size(), 0);
            for (NodeId n : *m) allow[n] = 1;
            hits = inline_search(h, q.data(), k, std::max(ef, k), allow);
        }
        for (auto& x : hits) out.emplace_back(x.first, x.second);
        return out;
    };
    return v;
}

} // namespace gv
