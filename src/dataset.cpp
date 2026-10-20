#include "graphvec/dataset.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace gv {

Matrix read_fvecs(const std::string& path, size_t max_rows) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    Matrix m;
    int32_t d;
    while (std::fread(&d, 4, 1, f) == 1) {
        if (m.dim == 0) m.dim = size_t(d);
        if (size_t(d) != m.dim) throw std::runtime_error("ragged fvecs");
        size_t off = m.data.size();
        m.data.resize(off + m.dim);
        if (std::fread(m.data.data() + off, 4, m.dim, f) != m.dim) throw std::runtime_error("short fvecs");
        if (++m.rows == max_rows) break;
    }
    std::fclose(f);
    return m;
}

std::vector<std::vector<int32_t>> read_ivecs(const std::string& path, size_t max_rows) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<std::vector<int32_t>> out;
    int32_t d;
    while (std::fread(&d, 4, 1, f) == 1) {
        std::vector<int32_t> r(static_cast<size_t>(d));
        if (std::fread(r.data(), 4, size_t(d), f) != size_t(d)) throw std::runtime_error("short ivecs");
        out.push_back(std::move(r));
        if (out.size() == max_rows) break;
    }
    std::fclose(f);
    return out;
}

void load_arxiv_graph(const std::string& dir, Graph& g) {
    std::ifstream nodes(dir + "/nodes.csv");
    if (!nodes) throw std::runtime_error("missing " + dir + "/nodes.csv (run scripts/prep_arxiv.py)");
    uint32_t paper = g.labels.intern("Paper");
    uint32_t k_id = g.keys.intern("id"), k_year = g.keys.intern("year"), k_subj = g.keys.intern("subject");
    std::string line;
    std::getline(nodes, line);  // header
    while (std::getline(nodes, line)) {
        long long id, year, subject;
        char cat[64];
        if (std::sscanf(line.c_str(), "%lld,%lld,%lld,%63s", &id, &year, &subject, cat) != 4)
            throw std::runtime_error("bad node row: " + line);
        if (size_t(id) != g.num_nodes()) throw std::runtime_error("node ids must be dense and ordered");
        g.create_node({paper, g.labels.intern(cat)},
                      {{k_id, Value(int64_t(id))}, {k_year, Value(int64_t(year))}, {k_subj, Value(int64_t(subject))}});
    }
    std::ifstream edges(dir + "/edges.csv");
    uint32_t cites = g.types.intern("CITES");
    std::getline(edges, line);
    while (std::getline(edges, line)) {
        unsigned s, d;
        if (std::sscanf(line.c_str(), "%u,%u", &s, &d) != 2) throw std::runtime_error("bad edge row");
        g.create_edge(s, d, cites);
    }
    g.create_index(k_id);
    g.compact();
}

Matrix load_arxiv_features(const std::string& dir) {
    std::ifstream in(dir + "/feats.f32", std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("missing feats.f32");
    size_t bytes = size_t(in.tellg());
    in.seekg(0);
    Matrix m;
    m.dim = 128;
    m.rows = bytes / (4 * m.dim);
    m.data.resize(m.rows * m.dim);
    in.read(reinterpret_cast<char*>(m.data.data()), std::streamsize(m.data.size() * 4));
    return m;
}

} // namespace gv
