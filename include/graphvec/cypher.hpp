// A Cypher subset: MATCH (patterns with labels, inline properties,
// direction, fixed and variable-length `*min..max` hops), WHERE, CREATE,
// RETURN [DISTINCT] with count/sum/min/max/avg, ORDER BY, SKIP, LIMIT, and
// CREATE INDEX ON :Label(prop).
//
// Not supported: OPTIONAL MATCH, WITH, UNWIND, MERGE, SET/DELETE, path
// variables, relationship properties, returning whole nodes (a node
// variable evaluates to its internal id).
#pragma once

#include "graphvec/graph.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gv {

struct Result {
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
    size_t nodes_created = 0, edges_created = 0;
    std::string plan;  // one line per operator, for EXPLAIN-style output
};

using Params = std::unordered_map<std::string, Value>;
// Writes are handed to `commit` as encoded records (see store.hpp) so a
// Store can log them; the default applies them straight to the graph.
using Commit = std::function<uint32_t(const std::string& record)>;

// Optional vector-search hook. Node i's vector is row i of the index. When
// set, a query of the form
//   MATCH (n ...) WHERE ... RETURN ... ORDER BY vec_dist(n, $q) LIMIT k
// is answered by one filtered top-k search: the graph side evaluates the
// labels/WHERE into `matches` (nullptr = no filter) and `search` picks a
// strategy (brute force over the matches, or filtered HNSW), naming it in
// `strategy`.
struct VectorHook {
    std::function<std::vector<std::pair<float, NodeId>>(
        const std::vector<float>& q, size_t k, const std::vector<NodeId>* matches, std::string& strategy)>
        search;
    std::function<float(NodeId, const std::vector<float>&)> dist;
};

struct CypherError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Result run_cypher(Graph& g, std::string_view query, const Params& params = {},
                  const Commit& commit = nullptr, const VectorHook* vec = nullptr);

} // namespace gv
