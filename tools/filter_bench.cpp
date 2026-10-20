// filter_bench: filtered top-10 search on ogbn-arxiv (169k papers, 128-d
// features). For each predicate, compare prefilter / postfilter / inline
// (see vector_hook.hpp) on recall@10 against the exact filtered answer.
//
// Query = a random paper's own vector, and the paper itself is always
// excluded ("papers similar to P"). Predicate families:
//   random    uncorrelated bitmap at a fixed selectivity (the control)
//   category  n:<category> for categories of very different sizes
//   year      n.year >= Y
//   graph     papers within 2 citation hops of P, from Cypher:
//             MATCH (p {id:$P})-[:CITES*1..2]-(q) RETURN DISTINCT q.id
//
//   filter_bench --data DIR [--nq 500] [--threads 4] [--out f.csv]
#include "graphvec/cypher.hpp"
#include "graphvec/dataset.hpp"
#include "graphvec/hnsw.hpp"
#include "graphvec/vector_hook.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>

using namespace gv;
using Clock = std::chrono::steady_clock;

static double secs(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

struct Strategy {
    std::string name;
    size_t ef;
};

int main(int argc, char** argv) {
    std::string data, out;
    size_t nq = 500;
    int threads = 4;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data") data = argv[++i];
        else if (a == "--nq") nq = std::stoul(argv[++i]);
        else if (a == "--threads") threads = std::stoi(argv[++i]);
        else if (a == "--out") out = argv[++i];
    }
    Graph g;
    load_arxiv_graph(data, g);
    Matrix x = load_arxiv_features(data);
    const size_t n = x.rows, k = 10;
    Hnsw h(x.dim, n, {16, 200, 100});
    auto t0 = Clock::now();
    h.build(x.data.data(), n, threads);
    std::fprintf(stderr, "built HNSW n=%zu in %.1fs\n", n, secs(t0));

    std::mt19937 rng(42);
    std::vector<uint32_t> queries;
    for (size_t i = 0; i < nq; ++i) queries.push_back(uint32_t(rng() % n));

    std::FILE* f = out.empty() ? stdout : std::fopen(out.c_str(), "w");
    std::fprintf(f, "family,param,selectivity,strategy,ef,recall10,qps,dist_per_query\n");

    const std::vector<Strategy> strategies = {
        {"prefilter", 0}, {"postfilter", 100}, {"postfilter", 1000}, {"inline", 100}};

    // Runs every strategy for one predicate family. allow_for(qi) fills the
    // bitmap + id list for query qi (the query paper is removed by caller).
    auto run_family = [&](const std::string& family, const std::string& param,
                          const std::function<void(size_t, std::vector<uint8_t>&, std::vector<uint32_t>&)>& allow_for) {
        std::vector<std::vector<uint8_t>> allow(nq);
        std::vector<std::vector<uint32_t>> ids(nq);
        double sel = 0;
        for (size_t qi = 0; qi < nq; ++qi) {
            allow[qi].assign(n, 0);
            allow_for(qi, allow[qi], ids[qi]);
            uint32_t self = queries[qi];
            if (allow[qi][self]) {
                allow[qi][self] = 0;
                ids[qi].erase(std::find(ids[qi].begin(), ids[qi].end(), self));
            }
            sel += double(ids[qi].size()) / double(n);
        }
        sel /= double(nq);
        // exact filtered answers
        std::vector<std::set<uint32_t>> truth(nq);
        for (size_t qi = 0; qi < nq; ++qi)
            for (auto& hit : prefilter_search(h, x.row(queries[qi]), k, ids[qi])) truth[qi].insert(hit.second);
        for (const Strategy& s : strategies) {
            size_t found = 0, want = 0;
            uint64_t dists = 0;
            auto t = Clock::now();
            std::vector<Hits> res(nq);
            for (size_t qi = 0; qi < nq; ++qi) {
                const float* q = x.row(queries[qi]);
                h.reset_distance_count();
                if (s.name == "prefilter") {
                    res[qi] = prefilter_search(h, q, k, ids[qi]);
                    dists += ids[qi].size();
                } else if (s.name == "postfilter") {
                    res[qi] = postfilter_search(h, q, k, s.ef, allow[qi]);
                } else {
                    res[qi] = inline_search(h, q, k, s.ef, allow[qi]);
                }
                dists += h.distance_count();
            }
            double el = secs(t);
            for (size_t qi = 0; qi < nq; ++qi) {
                want += truth[qi].size();
                for (auto& hit : res[qi]) found += truth[qi].count(hit.second);
            }
            std::fprintf(f, "%s,%s,%.6f,%s,%zu,%.4f,%.0f,%.0f\n", family.c_str(), param.c_str(), sel,
                         s.name.c_str(), s.ef, want ? double(found) / double(want) : 1.0, double(nq) / el,
                         double(dists) / double(nq));
            std::fflush(f);
        }
        std::fprintf(stderr, "%s %s sel=%.5f done\n", family.c_str(), param.c_str(), sel);
    };

    // ---- unfiltered baseline (only the query itself excluded) ----
    run_family("none", "-", [&](size_t, auto& a, auto& ids) {
        for (uint32_t i = 0; i < n; ++i) { a[i] = 1; ids.push_back(i); }
    });

    // ---- random, uncorrelated ----
    for (double s : {0.5, 0.1, 0.02, 0.005, 0.001, 0.0002}) {
        std::vector<uint8_t> bits(n);
        std::mt19937 r2(7);
        std::bernoulli_distribution b(s);
        for (auto& v : bits) v = b(r2);
        char p[32];
        std::snprintf(p, sizeof p, "%g", s);
        run_family("random", p, [&](size_t, auto& a, auto& ids) {
            for (uint32_t i = 0; i < n; ++i)
                if ((a[i] = bits[i])) ids.push_back(i);
        });
    }

    // ---- category label, via Cypher label scan ----
    {
        std::vector<std::pair<size_t, std::string>> cats;
        for (uint32_t l = 0; l < g.labels.size(); ++l)
            if (g.labels.name(l) != "Paper") cats.emplace_back(g.nodes_with_label(l).size(), g.labels.name(l));
        std::sort(cats.begin(), cats.end());
        // 6 categories spread from the smallest to the largest
        for (size_t j = 0; j < 6; ++j) {
            const auto& c = cats[j * (cats.size() - 1) / 5];
            Result r = run_cypher(g, "MATCH (n:" + c.second + ") RETURN n.id");
            std::vector<uint32_t> base;
            for (auto& row : r.rows) base.push_back(uint32_t(row[0].as_int()));
            run_family("category", c.second, [&](size_t, auto& a, auto& ids) {
                ids = base;
                for (uint32_t i : ids) a[i] = 1;
            });
        }
    }

    // ---- year >= Y ----
    for (int y : {2010, 2015, 2017, 2018, 2019, 2020}) {
        Result r = run_cypher(g, "MATCH (n:Paper) WHERE n.year >= " + std::to_string(y) + " RETURN n.id");
        std::vector<uint32_t> base;
        for (auto& row : r.rows) base.push_back(uint32_t(row[0].as_int()));
        run_family("year", ">=" + std::to_string(y), [&](size_t, auto& a, auto& ids) {
            ids = base;
            for (uint32_t i : ids) a[i] = 1;
        });
    }

    // ---- graph neighbourhood of the query paper (correlated) ----
    {
        double cy_ms = 0;
        run_family("graph", "2hop", [&](size_t qi, auto& a, auto& ids) {
            auto t = Clock::now();
            Result r = run_cypher(g, "MATCH (p:Paper {id: $P})-[:CITES*1..2]-(q) RETURN DISTINCT q.id",
                                  {{"P", Value(int64_t(queries[qi]))}});
            cy_ms += secs(t) * 1e3;
            for (auto& row : r.rows) {
                uint32_t id = uint32_t(row[0].as_int());
                if (!a[id]) { a[id] = 1; ids.push_back(id); }
            }
        });
        std::fprintf(stderr, "graph predicate (Cypher) mean %.3f ms/query\n", cy_ms / double(nq));
        std::fprintf(f, "# graph predicate evaluation mean_ms=%.3f\n", cy_ms / double(nq));
    }
    if (f != stdout) std::fclose(f);
}
