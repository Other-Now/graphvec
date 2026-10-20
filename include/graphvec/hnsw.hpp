// HNSW (Malkov & Yashunin, 2016), written from scratch.
//
// Layout follows the paper's recommendations and hnswlib's practice:
// level 0 is one flat array of fixed-size neighbour lists (M0 = 2M slots),
// upper levels are small per-node arrays, vectors are one contiguous block.
// Neighbour lists are pruned with the paper's heuristic (Algorithm 4).
// Insertion is thread-safe with one mutex per node plus a global lock that
// is held only by an insert that raises the top level.
//
// search_filtered() is in-search filtering: the traversal walks every node,
// but only nodes accepted by the predicate enter the result set.
#pragma once

#include "graphvec/distance.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace gv {

class Hnsw {
public:
    struct Params {
        int M = 16;
        int ef_construction = 200;
        uint64_t seed = 100;
    };
    using Hit = std::pair<float, uint32_t>;  // (squared distance, id)

    Hnsw(size_t dim, size_t capacity, Params p)
        : dim_(dim), cap_(capacity), M_(p.M), M0_(2 * p.M), efc_(p.ef_construction), seed_(p.seed),
          mult_(1.0 / std::log(double(p.M))), data_(capacity * dim), link0_(capacity * (M0_ + 1), 0),
          upper_(capacity), level_(capacity, 0), locks_(new std::mutex[capacity]) {}

    size_t size() const { return count_.load(); }
    size_t dim() const { return dim_; }
    const float* vec(uint32_t id) const { return data_.data() + size_t(id) * dim_; }
    float dist(uint32_t id, const float* q) const { return l2sq(vec(id), q, dim_); }

    size_t memory_bytes() const {
        size_t b = data_.size() * sizeof(float) + link0_.size() * sizeof(uint32_t) +
                   upper_.size() * sizeof(upper_[0]) + level_.size() * sizeof(int);
        for (auto& u : upper_) b += u.capacity() * sizeof(uint32_t);
        return b;
    }

    // Insert n vectors in parallel; ids are 0..n-1 in input order.
    void build(const float* data, size_t n, int threads) {
        if (n > cap_) throw std::runtime_error("capacity exceeded");
        if (n == 0) return;
        add(data, 0);
        std::atomic<size_t> next{1};
        auto work = [&] {
            for (size_t i; (i = next.fetch_add(1)) < n;) add(data + i * dim_, uint32_t(i));
        };
        std::vector<std::thread> ts;
        for (int t = 1; t < threads; ++t) ts.emplace_back(work);
        work();
        for (auto& t : ts) t.join();
    }

    std::vector<Hit> search(const float* q, size_t k, size_t ef) const {
        return search_filtered(q, k, ef, [](uint32_t) { return true; });
    }

    template <class Filter>
    std::vector<Hit> search_filtered(const float* q, size_t k, size_t ef, const Filter& allow) const {
        if (count_.load() == 0) return {};
        uint32_t ep = entry_;
        for (int l = max_level_; l > 0; --l) ep = greedy(q, ep, l, false);
        auto top = search_layer<false>(q, ep, std::max(ef, k), 0, allow);
        std::vector<Hit> out;
        while (top.size() > k) top.pop();
        while (!top.empty()) { out.push_back(top.top()); top.pop(); }
        std::reverse(out.begin(), out.end());
        return out;
    }

    // Distance computations done by search calls since the last reset.
    uint64_t distance_count() const { return ndist_; }
    void reset_distance_count() const { ndist_ = 0; }

private:
    using MaxHeap = std::priority_queue<Hit>;
    using MinHeap = std::priority_queue<Hit, std::vector<Hit>, std::greater<Hit>>;

    int max_links(int level) const { return level == 0 ? M0_ : M_; }

    // Neighbour list of `id` at `level`: [count, ids...].
    uint32_t* links(uint32_t id, int level) {
        return level == 0 ? &link0_[size_t(id) * (M0_ + 1)] : &upper_[id][size_t(level - 1) * (M_ + 1)];
    }
    const uint32_t* links(uint32_t id, int level) const {
        return const_cast<Hnsw*>(this)->links(id, level);
    }

    // Level from a hash of the id: deterministic and needs no shared RNG.
    int random_level(uint32_t id) const {
        uint64_t x = (uint64_t(id) + 1) * 0x9e3779b97f4a7c15ULL ^ seed_;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        x ^= x >> 31;
        double u = (double(x >> 11) + 0.5) * (1.0 / 9007199254740992.0);
        return int(-std::log(u) * mult_);
    }

    // Per-thread visited set with epoch tags, so clearing is O(1).
    struct Visited {
        std::vector<uint16_t> tag;
        uint16_t epoch = 0;
    };
    Visited& visited() const {
        thread_local Visited v;
        thread_local const Hnsw* owner = nullptr;
        if (owner != this || v.tag.size() < cap_) {
            v.tag.assign(cap_, 0);
            v.epoch = 0;
            owner = this;
        }
        if (++v.epoch == 0) {
            std::fill(v.tag.begin(), v.tag.end(), 0);
            v.epoch = 1;
        }
        return v;
    }

    uint32_t greedy(const float* q, uint32_t ep, int level, bool lock) const {
        float best = l2sq(q, vec(ep), dim_);
        if (!lock) ++ndist_;
        for (bool changed = true; changed;) {
            changed = false;
            uint32_t buf[64 + 1];
            const uint32_t* l = links(ep, level);
            if (lock) {
                std::lock_guard<std::mutex> g(locks_[ep]);
                std::memcpy(buf, l, (l[0] + 1) * sizeof(uint32_t));
                l = buf;
            }
            for (uint32_t j = 1; j <= l[0]; ++j) {
                float d = l2sq(q, vec(l[j]), dim_);
                if (!lock) ++ndist_;
                if (d < best) { best = d; ep = l[j]; changed = true; }
            }
        }
        return ep;
    }

    // Algorithm 2: beam search of width ef on one level. Returns a max-heap
    // of the ef closest *accepted* nodes. While building (Lock = true) each
    // neighbour list is copied under that node's mutex.
    template <bool Lock, class Filter>
    MaxHeap search_layer(const float* q, uint32_t ep, size_t ef, int level, const Filter& allow) const {
        Visited& vis = visited();
        MaxHeap top;
        MinHeap cand;
        float d0 = l2sq(q, vec(ep), dim_);
        if (!Lock) ++ndist_;
        vis.tag[ep] = vis.epoch;
        cand.push({d0, ep});
        if (allow(ep)) top.push({d0, ep});
        std::vector<uint32_t> buf(M0_ + 1);
        while (!cand.empty()) {
            auto [d, c] = cand.top();
            if (top.size() >= ef && d > top.top().first) break;
            cand.pop();
            const uint32_t* l = links(c, level);
            if (Lock) {
                std::lock_guard<std::mutex> g(locks_[c]);
                std::memcpy(buf.data(), l, (l[0] + 1) * sizeof(uint32_t));
                l = buf.data();
            }
            for (uint32_t j = 1; j <= l[0]; ++j) {
                uint32_t n = l[j];
#if GV_AVX2
                if (j + 1 <= l[0]) _mm_prefetch(reinterpret_cast<const char*>(vec(l[j + 1])), _MM_HINT_T0);
#endif
                if (vis.tag[n] == vis.epoch) continue;
                vis.tag[n] = vis.epoch;
                float dn = l2sq(q, vec(n), dim_);
                if (!Lock) ++ndist_;
                if (top.size() < ef || dn < top.top().first) {
                    cand.push({dn, n});
                    if (allow(n)) {
                        top.push({dn, n});
                        if (top.size() > ef) top.pop();
                    }
                }
            }
        }
        return top;
    }

    // Algorithm 4 (simple variant, no extension, no keepPruned): walk
    // candidates nearest-first and keep one only if it is closer to the base
    // than to every neighbour kept so far. This spreads the edges across
    // directions instead of all pointing into the nearest cluster.
    std::vector<uint32_t> select(MinHeap cand, size_t m) const {
        std::vector<uint32_t> keep;
        while (!cand.empty() && keep.size() < m) {
            auto [d, c] = cand.top();
            cand.pop();
            bool good = true;
            for (uint32_t r : keep)
                if (l2sq(vec(c), vec(r), dim_) < d) { good = false; break; }
            if (good) keep.push_back(c);
        }
        return keep;
    }

    void add(const float* v, uint32_t id) {
        std::memcpy(data_.data() + size_t(id) * dim_, v, dim_ * sizeof(float));
        int level = random_level(id);
        level_[id] = level;
        if (level > 0) upper_[id].assign(size_t(level) * (M_ + 1), 0);

        std::unique_lock<std::mutex> top_lock(global_);
        if (count_.load() == 0) {
            entry_ = id;
            max_level_ = level;
            count_.store(1);
            return;
        }
        int maxl = max_level_;
        uint32_t ep = entry_;
        if (level <= maxl) top_lock.unlock();  // only a new top level needs the lock held

        const float* q = vec(id);
        for (int l = maxl; l > level; --l) ep = greedy(q, ep, l, true);
        auto all = [](uint32_t) { return true; };
        for (int l = std::min(level, maxl); l >= 0; --l) {
            MaxHeap w = search_layer<true>(q, ep, size_t(efc_), l, all);
            MinHeap c;
            while (!w.empty()) { c.push(w.top()); w.pop(); }
            ep = c.top().second;
            std::vector<uint32_t> nb = select(c, size_t(M_));
            {
                std::lock_guard<std::mutex> g(locks_[id]);
                uint32_t* mine = links(id, l);
                mine[0] = uint32_t(nb.size());
                std::copy(nb.begin(), nb.end(), mine + 1);
            }
            for (uint32_t n : nb) connect(n, id, l);
        }
        count_.fetch_add(1);
        if (level > maxl) {
            entry_ = id;
            max_level_ = level;
        }
    }

    // Add the back-edge n -> id; if n's list is full, re-prune it.
    void connect(uint32_t n, uint32_t id, int level) {
        std::lock_guard<std::mutex> g(locks_[n]);
        uint32_t* l = links(n, level);
        size_t cap = size_t(max_links(level));
        for (uint32_t j = 1; j <= l[0]; ++j)
            if (l[j] == id) return;
        if (l[0] < cap) {
            l[++l[0]] = id;
            return;
        }
        MinHeap c;
        c.push({l2sq(vec(n), vec(id), dim_), id});
        for (uint32_t j = 1; j <= l[0]; ++j) c.push({l2sq(vec(n), vec(l[j]), dim_), l[j]});
        std::vector<uint32_t> nb = select(std::move(c), cap);
        l[0] = uint32_t(nb.size());
        std::copy(nb.begin(), nb.end(), l + 1);
    }

    size_t dim_, cap_;
    int M_, M0_, efc_;
    uint64_t seed_;
    double mult_;
    std::vector<float> data_;
    std::vector<uint32_t> link0_;
    std::vector<std::vector<uint32_t>> upper_;
    std::vector<int> level_;
    std::unique_ptr<std::mutex[]> locks_;
    std::mutex global_;
    uint32_t entry_ = 0;
    int max_level_ = 0;
    std::atomic<size_t> count_{0};
    mutable uint64_t ndist_ = 0;
};

} // namespace gv
