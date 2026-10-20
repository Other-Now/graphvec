// gvsh: load ogbn-arxiv and run Cypher.
//   gvsh --data DIR -e "MATCH ... RETURN ..."      one query, table output
//   gvsh --data DIR --queries FILE --out FILE       one query per line, JSON lines
//   add --explain to print the plan, --vectors to enable vec_dist()
#include "graphvec/cypher.hpp"
#include "graphvec/dataset.hpp"
#include "graphvec/hnsw.hpp"
#include "graphvec/vector_hook.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

using namespace gv;
using Clock = std::chrono::steady_clock;

static std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o + "\"";
}

static std::string json(const Value& v) {
    if (v.is_null()) return "null";
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_int()) return std::to_string(v.as_int());
    if (v.is_num()) {
        if (!std::isfinite(v.as_num())) return "null";
        char b[32];
        std::snprintf(b, sizeof b, "%.17g", v.as_num());
        // keep floats recognisable as floats
        std::string s = b;
        if (s.find_first_of(".eE") == std::string::npos) s += ".0";
        return s;
    }
    if (v.is_str()) return json_str(v.as_str());
    std::string s = "[";
    for (size_t i = 0; i < v.as_list().size(); ++i) s += (i ? "," : "") + json(v.as_list()[i]);
    return s + "]";
}

int main(int argc, char** argv) {
    std::string data, query, qfile, out;
    bool explain = false, vectors = false;
    int repeat = 1;  // -e: run the query this many times (profiling), report the last
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--data") data = val();
        else if (a == "-e") query = val();
        else if (a == "--queries") qfile = val();
        else if (a == "--out") out = val();
        else if (a == "--explain") explain = true;
        else if (a == "--vectors") vectors = true;
        else if (a == "--repeat") repeat = std::stoi(val());
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }
    if (data.empty()) { std::fprintf(stderr, "need --data DIR\n"); return 2; }

    Graph g;
    auto t0 = Clock::now();
    load_arxiv_graph(data, g);
    std::fprintf(stderr, "loaded %zu nodes, %zu edges in %.2f s\n", g.num_nodes(), g.num_edges(),
                 std::chrono::duration<double>(Clock::now() - t0).count());

    std::unique_ptr<Hnsw> index;
    Matrix feats;
    VectorHook hook;
    if (vectors) {
        feats = load_arxiv_features(data);
        index = std::make_unique<Hnsw>(feats.dim, feats.rows, Hnsw::Params{16, 200, 100});
        t0 = Clock::now();
        index->build(feats.data.data(), feats.rows, 4);
        std::fprintf(stderr, "built HNSW over %zu vectors in %.1f s\n", feats.rows,
                     std::chrono::duration<double>(Clock::now() - t0).count());
        hook = make_vector_hook(*index, 64, 2000);
    }

    if (!query.empty()) {
        Params params;
        if (vectors) {
            // $q = feature vector of paper 0, for quick manual tries
            List q;
            for (size_t j = 0; j < feats.dim; ++j) q.emplace_back(double(feats.row(0)[j]));
            params["q"] = Value(q);
        }
        Result r;
        double ms = 0;
        for (int i = 0; i < repeat; ++i) {
            auto t = Clock::now();
            r = run_cypher(g, query, params, nullptr, vectors ? &hook : nullptr);
            ms = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        }
        if (explain) std::cout << r.plan;
        for (size_t c = 0; c < r.columns.size(); ++c) std::cout << (c ? "\t" : "") << r.columns[c];
        std::cout << "\n";
        for (auto& row : r.rows) {
            for (size_t c = 0; c < row.size(); ++c) std::cout << (c ? "\t" : "") << row[c].to_string();
            std::cout << "\n";
        }
        std::fprintf(stderr, "%zu rows, %.3f ms\n", r.rows.size(), ms);
        return 0;
    }

    std::ifstream in(qfile);
    std::ofstream os(out);
    std::string line;
    size_t qi = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::string rec = "{\"q\":" + std::to_string(qi++) + ",\"query\":" + json_str(line);
        auto t = Clock::now();
        try {
            Result r = run_cypher(g, line);
            double ms = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
            rec += ",\"ms\":" + std::to_string(ms) + ",\"cols\":[";
            for (size_t c = 0; c < r.columns.size(); ++c) rec += (c ? "," : "") + json_str(r.columns[c]);
            rec += "],\"rows\":[";
            for (size_t i = 0; i < r.rows.size(); ++i) {
                rec += i ? ",[" : "[";
                for (size_t c = 0; c < r.rows[i].size(); ++c) rec += (c ? "," : "") + json(r.rows[i][c]);
                rec += "]";
            }
            rec += "],\"plan\":" + json_str(r.plan) + "}";
        } catch (const std::exception& e) {
            rec += ",\"error\":" + json_str(e.what()) + "}";
        }
        os << rec << "\n";
    }
    std::fprintf(stderr, "ran %zu queries\n", qi);
}
