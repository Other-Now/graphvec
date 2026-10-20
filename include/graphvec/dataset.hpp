// Loaders for the two public datasets the benchmarks use.
#pragma once

#include "graphvec/graph.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gv {

struct Matrix {
    size_t rows = 0, dim = 0;
    std::vector<float> data;
    const float* row(size_t i) const { return data.data() + i * dim; }
};

// TEXMEX .fvecs/.ivecs (SIFT1M): each row is [int32 dim][dim values].
// max_rows = 0 reads everything.
Matrix read_fvecs(const std::string& path, size_t max_rows = 0);
std::vector<std::vector<int32_t>> read_ivecs(const std::string& path, size_t max_rows = 0);

// ogbn-arxiv as written by scripts/prep_arxiv.py: every paper becomes a
// node labelled :Paper and :<category> with properties id, year, subject;
// every citation a :CITES edge. Node id i == paper id i == feature row i.
void load_arxiv_graph(const std::string& dir, Graph& g);
Matrix load_arxiv_features(const std::string& dir);

} // namespace gv
