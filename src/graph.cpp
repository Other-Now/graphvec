#include "graphvec/graph.hpp"

#include "graphvec/codec.hpp"

#include <algorithm>
#include <stdexcept>

namespace gv {

uint32_t Dict::intern(std::string_view s) {
    auto it = ids_.find(std::string(s));
    if (it != ids_.end()) return it->second;
    uint32_t id = uint32_t(names_.size());
    names_.emplace_back(s);
    ids_.emplace(names_.back(), id);
    return id;
}

uint32_t Dict::find(std::string_view s) const {
    auto it = ids_.find(std::string(s));
    return it == ids_.end() ? UINT32_MAX : it->second;
}

static const std::vector<NodeId> kEmpty;
static const Value kNull;

NodeId Graph::create_node(const std::vector<uint32_t>& labels, Props props) {
    NodeId n = NodeId(labels_.size());
    uint64_t mask = 0;
    for (uint32_t l : labels) {
        if (l >= kMaxLabels) throw std::runtime_error("more than 64 labels");
        if (!((mask >> l) & 1)) {
            if (label_nodes_.size() <= l) label_nodes_.resize(l + 1);
            label_nodes_[l].push_back(n);
        }
        mask |= uint64_t{1} << l;
    }
    std::sort(props.begin(), props.end(), [](auto& a, auto& b) { return a.first < b.first; });
    labels_.push_back(mask);
    props_.push_back(std::move(props));
    delta_out_.emplace_back();
    delta_in_.emplace_back();
    for (auto& [k, v] : props_.back()) index_insert(k, v, n);
    return n;
}

EdgeId Graph::create_edge(NodeId src, NodeId dst, uint32_t type) {
    if (src >= num_nodes() || dst >= num_nodes()) throw std::runtime_error("edge endpoint missing");
    EdgeId e = EdgeId(edge_src_.size());
    edge_src_.push_back(src);
    edge_dst_.push_back(dst);
    edge_type_.push_back(type);
    delta_out_[src].push_back({dst, e, type});
    delta_in_[dst].push_back({src, e, type});
    ++delta_count_;
    return e;
}

void Graph::set_prop(NodeId n, uint32_t key, Value v) {
    Props& ps = props_.at(n);
    auto it = std::lower_bound(ps.begin(), ps.end(), key, [](auto& p, uint32_t k) { return p.first < k; });
    if (it != ps.end() && it->first == key) {
        index_erase(key, it->second, n);
        it->second = std::move(v);
    } else {
        it = ps.insert(it, {key, std::move(v)});
    }
    index_insert(key, it->second, n);
}

void Graph::create_index(uint32_t key) {
    if (index_.count(key)) return;
    index_[key];
    for (NodeId n = 0; n < num_nodes(); ++n) {
        const Value& v = prop(n, key);
        if (!v.is_null()) index_[key][v].push_back(n);
    }
}

void Graph::index_insert(uint32_t key, const Value& v, NodeId n) {
    auto it = index_.find(key);
    if (it != index_.end() && !v.is_null()) it->second[v].push_back(n);
}

void Graph::index_erase(uint32_t key, const Value& v, NodeId n) {
    auto it = index_.find(key);
    if (it == index_.end() || v.is_null()) return;
    auto& vec = it->second[v];
    vec.erase(std::remove(vec.begin(), vec.end(), n), vec.end());
}

const std::vector<NodeId>& Graph::nodes_with_label(uint32_t label) const {
    return label < label_nodes_.size() ? label_nodes_[label] : kEmpty;
}

const Value& Graph::prop(NodeId n, uint32_t key) const {
    const Props& ps = props_[n];
    auto it = std::lower_bound(ps.begin(), ps.end(), key, [](auto& p, uint32_t k) { return p.first < k; });
    return it != ps.end() && it->first == key ? it->second : kNull;
}

const std::vector<NodeId>& Graph::index_lookup(uint32_t key, const Value& v) const {
    auto it = index_.find(key);
    if (it == index_.end()) return kEmpty;
    auto jt = it->second.find(v);
    return jt == it->second.end() ? kEmpty : jt->second;
}

void Graph::compact() {
    // Counting sort of all edges by endpoint, in edge-id order, so iteration
    // order after compaction is identical to before it.
    auto build = [&](Csr& c, const std::vector<NodeId>& from, const std::vector<NodeId>& to) {
        size_t n = num_nodes();
        c.off.assign(n + 1, 0);
        for (NodeId s : from) ++c.off[s + 1];
        for (size_t i = 0; i < n; ++i) c.off[i + 1] += c.off[i];
        c.adj.resize(from.size());
        std::vector<uint32_t> pos(c.off.begin(), c.off.end() - 1);
        for (EdgeId e = 0; e < from.size(); ++e) c.adj[pos[from[e]]++] = {to[e], e, edge_type_[e]};
    };
    build(out_, edge_src_, edge_dst_);
    build(in_, edge_dst_, edge_src_);
    for (auto& d : delta_out_) d.clear();
    for (auto& d : delta_in_) d.clear();
    delta_count_ = 0;
}

std::string Graph::serialize() const {
    Writer w;
    auto dict = [&](const Dict& d) {
        w.u32(uint32_t(d.size()));
        for (uint32_t i = 0; i < d.size(); ++i) w.str(d.name(i));
    };
    dict(labels);
    dict(types);
    dict(keys);
    w.u32(uint32_t(num_nodes()));
    for (NodeId n = 0; n < num_nodes(); ++n) {
        w.u64(labels_[n]);
        w.u32(uint32_t(props_[n].size()));
        for (auto& [k, v] : props_[n]) { w.u32(k); w.value(v); }
    }
    w.u32(uint32_t(num_edges()));
    for (EdgeId e = 0; e < num_edges(); ++e) { w.u32(edge_src_[e]); w.u32(edge_dst_[e]); w.u32(edge_type_[e]); }
    w.u32(uint32_t(index_.size()));
    for (auto& [k, _] : index_) w.u32(k);
    return std::move(w.buf);
}

void Graph::deserialize(std::string_view bytes) {
    *this = Graph();
    Reader r{bytes};
    auto dict = [&](Dict& d) {
        uint32_t n = r.u32();
        for (uint32_t i = 0; i < n; ++i) d.intern(r.str());
    };
    dict(labels);
    dict(types);
    dict(keys);
    uint32_t nn = r.u32();
    for (uint32_t i = 0; i < nn; ++i) {
        uint64_t mask = r.u64();
        std::vector<uint32_t> ls;
        for (uint32_t l = 0; l < kMaxLabels; ++l)
            if ((mask >> l) & 1) ls.push_back(l);
        Props ps(r.u32());
        for (auto& p : ps) { p.first = r.u32(); p.second = r.value(); }
        create_node(ls, std::move(ps));
    }
    uint32_t ne = r.u32();
    for (uint32_t i = 0; i < ne; ++i) {
        NodeId s = r.u32(), d = r.u32();
        create_edge(s, d, r.u32());
    }
    uint32_t ni = r.u32();
    for (uint32_t i = 0; i < ni; ++i) create_index(r.u32());
    compact();
}

} // namespace gv
