// Property graph storage: CSR adjacency for reads plus a per-node delta for
// writes, merged by compact(). Nodes carry a label bitmask and a small sorted
// property vector; edges carry a type. Deletes are not supported.
#pragma once

#include "graphvec/value.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gv {

using NodeId = uint32_t;
using EdgeId = uint32_t;
constexpr NodeId kNoNode = UINT32_MAX;

// String <-> dense id dictionary for labels, edge types and property keys.
class Dict {
public:
    uint32_t intern(std::string_view s);
    // Returns UINT32_MAX for an unknown name (a query naming a label that
    // was never created matches nothing).
    uint32_t find(std::string_view s) const;
    const std::string& name(uint32_t id) const { return names_[id]; }
    size_t size() const { return names_.size(); }

private:
    std::vector<std::string> names_;
    std::unordered_map<std::string, uint32_t> ids_;
};

struct Adj {
    NodeId nbr;
    EdgeId eid;
    uint32_t type;
};

using Props = std::vector<std::pair<uint32_t, Value>>;  // sorted by key id

class Graph {
public:
    static constexpr int kMaxLabels = 64;

    // ---- writes ----
    NodeId create_node(const std::vector<uint32_t>& labels, Props props);
    EdgeId create_edge(NodeId src, NodeId dst, uint32_t type);
    void set_prop(NodeId n, uint32_t key, Value v);
    void create_index(uint32_t key);  // hash index on a property key
    // Merge per-node deltas into the CSR arrays.
    void compact();

    // ---- reads ----
    size_t num_nodes() const { return labels_.size(); }
    size_t num_edges() const { return edge_src_.size(); }
    size_t delta_edges() const { return delta_count_; }
    bool has_label(NodeId n, uint32_t label) const {
        return label < kMaxLabels && (labels_[n] >> label) & 1;
    }
    uint64_t label_mask(NodeId n) const { return labels_[n]; }
    const std::vector<NodeId>& nodes_with_label(uint32_t label) const;
    const Value& prop(NodeId n, uint32_t key) const;
    const Props& props(NodeId n) const { return props_[n]; }
    bool indexed(uint32_t key) const { return index_.count(key) != 0; }
    // Nodes whose `key` property equals v (index must exist).
    const std::vector<NodeId>& index_lookup(uint32_t key, const Value& v) const;
    EdgeId edge_src(EdgeId e) const { return edge_src_[e]; }
    EdgeId edge_dst(EdgeId e) const { return edge_dst_[e]; }
    uint32_t edge_type(EdgeId e) const { return edge_type_[e]; }

    // Visit adjacency: CSR range first, then the delta written since the last
    // compaction. f(const Adj&) returns false to stop early.
    template <class F>
    bool for_each_out(NodeId n, F&& f) const { return visit(out_, delta_out_, n, f); }
    template <class F>
    bool for_each_in(NodeId n, F&& f) const { return visit(in_, delta_in_, n, f); }
    size_t out_degree(NodeId n) const { return degree(out_, delta_out_, n); }
    size_t in_degree(NodeId n) const { return degree(in_, delta_in_, n); }

    Dict labels, types, keys;

    // Snapshot (de)serialization of the whole graph; used by the WAL layer.
    std::string serialize() const;
    void deserialize(std::string_view bytes);

private:
    struct Csr {
        std::vector<uint32_t> off{0};  // off[n]..off[n+1] is node n's range
        std::vector<Adj> adj;
    };
    template <class F>
    static bool visit(const Csr& c, const std::vector<std::vector<Adj>>& d, NodeId n, F& f) {
        if (n + 1 < c.off.size())
            for (uint32_t i = c.off[n]; i < c.off[n + 1]; ++i)
                if (!f(c.adj[i])) return false;
        for (const Adj& a : d[n])
            if (!f(a)) return false;
        return true;
    }
    static size_t degree(const Csr& c, const std::vector<std::vector<Adj>>& d, NodeId n) {
        size_t base = n + 1 < c.off.size() ? c.off[n + 1] - c.off[n] : 0;
        return base + d[n].size();
    }
    void index_insert(uint32_t key, const Value& v, NodeId n);
    void index_erase(uint32_t key, const Value& v, NodeId n);

    std::vector<uint64_t> labels_;
    std::vector<Props> props_;
    std::vector<std::vector<NodeId>> label_nodes_;
    std::vector<NodeId> edge_src_, edge_dst_;
    std::vector<uint32_t> edge_type_;
    Csr out_, in_;
    std::vector<std::vector<Adj>> delta_out_, delta_in_;
    size_t delta_count_ = 0;
    std::unordered_map<uint32_t, std::unordered_map<Value, std::vector<NodeId>, ValueHash>> index_;
};

} // namespace gv
