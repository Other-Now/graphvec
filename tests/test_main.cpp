// Unit tests: storage, WAL recovery, Cypher semantics, HNSW recall.
#include "graphvec/cypher.hpp"
#include "graphvec/hnsw.hpp"
#include "graphvec/ring.hpp"
#include "graphvec/store.hpp"
#include "graphvec/vector_hook.hpp"

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>

using namespace gv;
namespace fs = std::filesystem;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++checks;                                                                     \
        if (!(cond)) {                                                                \
            ++failures;                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                             \
    } while (0)

static std::string cell(const Result& r, size_t row, size_t col) { return r.rows.at(row).at(col).to_string(); }

static std::string column(const Result& r, size_t col = 0) {
    std::string s;
    for (size_t i = 0; i < r.rows.size(); ++i) s += (i ? "," : "") + r.rows[i][col].to_string();
    return s;
}

// a -KNOWS-> b -KNOWS-> c -KNOWS-> a,  a -LIKES-> c,  c -KNOWS-> d
static Graph small() {
    Graph g;
    run_cypher(g, "CREATE (a:Person {name: 'a', age: 30}), (b:Person {name: 'b', age: 20}), "
                  "(c:Person {name: 'c', age: 40}), (d:Robot {name: 'd'})");
    run_cypher(g, "MATCH (a {name: 'a'}), (b {name: 'b'}), (c {name: 'c'}), (d {name: 'd'}) "
                  "CREATE (a)-[:KNOWS]->(b), (b)-[:KNOWS]->(c), (c)-[:KNOWS]->(a), (a)-[:LIKES]->(c), "
                  "(c)-[:KNOWS]->(d)");
    return g;
}

static void test_cypher() {
    Graph g = small();
    CHECK(g.num_nodes() == 4 && g.num_edges() == 5);
    CHECK(cell(run_cypher(g, "MATCH (x)-[:KNOWS]->(y) RETURN count(*)"), 0, 0) == "4");
    CHECK(cell(run_cypher(g, "MATCH (x)-->(y) RETURN count(*)"), 0, 0) == "5");
    CHECK(column(run_cypher(g, "MATCH (x {name:'a'})-[:KNOWS*1..3]->(y) RETURN y.name ORDER BY y.name")) ==
          "a,b,c,d");
    // Relationship uniqueness: the walk a->b->c->a cannot reuse a->b.
    CHECK(cell(run_cypher(g, "MATCH (x {name:'a'})-[:KNOWS*1..6]->(y) RETURN count(*)"), 0, 0) == "4");
    CHECK(cell(run_cypher(g, "MATCH (x {name:'b'})--(y) RETURN count(*)"), 0, 0) == "2");
    CHECK(cell(run_cypher(g, "MATCH (x {name:'a'})-[:KNOWS|LIKES]->(y) RETURN count(*)"), 0, 0) == "2");
    Result agg = run_cypher(g, "MATCH (p:Person) RETURN count(*), sum(p.age), min(p.age), max(p.age), avg(p.age)");
    CHECK(column(agg, 0) == "3" && cell(agg, 0, 1) == "90" && cell(agg, 0, 2) == "20" && cell(agg, 0, 3) == "40" &&
          cell(agg, 0, 4) == "30");
    Result grp = run_cypher(g, "MATCH (x)-[:KNOWS]->(y) RETURN x.name AS n, count(*) AS c ORDER BY c DESC, n");
    CHECK(column(grp, 0) == "c,a,b" && column(grp, 1) == "2,1,1");
    // NULL OR NULL is null, so d (no age, name 'd') is filtered out.
    CHECK(column(run_cypher(g, "MATCH (p) WHERE p.age > 25 OR p.name = 'b' RETURN p.name ORDER BY p.name")) ==
          "a,b,c");
    CHECK(cell(run_cypher(g, "MATCH (x)-[:KNOWS]->(y)-[:KNOWS]->(z)-[:KNOWS]->(x) RETURN count(*)"), 0, 0) == "3");
    CHECK(column(run_cypher(g, "MATCH (p:Person) RETURN p.name ORDER BY p.age DESC SKIP 1 LIMIT 1")) == "a");
    CHECK(column(run_cypher(g, "MATCH (p) WHERE p.name IN ['b', 'd', 'z'] RETURN p.name ORDER BY p.name")) == "b,d");
    CHECK(column(run_cypher(g, "MATCH (x)-[:KNOWS*1..2]->(y) RETURN DISTINCT y.name ORDER BY y.name")) == "a,b,c,d");
    CHECK(cell(run_cypher(g, "MATCH (p:NoSuchLabel) RETURN count(*)"), 0, 0) == "0");
    CHECK(run_cypher(g, "MATCH (p:Person) RETURN p.name LIMIT 2").rows.size() == 2);
    CHECK(cell(run_cypher(g, "MATCH (a:Person)-[:LIKES]->(b) RETURN count(*)"), 0, 0) == "1");
    CHECK(cell(run_cypher(g, "MATCH (p {name:'d'}) RETURN p.age IS NULL"), 0, 0) == "true");
    CHECK(cell(run_cypher(g, "MATCH (p:Person) WHERE NOT p.age < 30 RETURN count(*)"), 0, 0) == "2");
    CHECK(cell(run_cypher(g, "MATCH (p:Person) WHERE p.age >= 20 AND p.age <= 30 RETURN count(*)"), 0, 0) == "2");

    // Planning: the label with fewer nodes is the start; an index beats both.
    Result plan = run_cypher(g, "MATCH (a:Person)-[:KNOWS]->(r:Robot) RETURN count(*)");
    CHECK(plan.plan.rfind("NodeByLabelScan(r:Robot)", 0) == 0);
    CHECK(cell(plan, 0, 0) == "1");
    run_cypher(g, "CREATE INDEX ON :Person(name)");
    Result seek = run_cypher(g, "MATCH (a:Person)-[:KNOWS]->(b) WHERE a.name = 'c' RETURN b.name ORDER BY b.name");
    CHECK(seek.plan.rfind("NodeIndexSeek(a.name)", 0) == 0);
    CHECK(column(seek) == "a,d");

    // Writes landing in the delta are visible before and after compaction.
    run_cypher(g, "MATCH (d {name:'d'}), (b {name:'b'}) CREATE (d)-[:KNOWS]->(b)");
    std::string before = column(run_cypher(g, "MATCH (x {name:'a'})-[:KNOWS*1..4]->(y) RETURN y.name ORDER BY y.name"));
    g.compact();
    std::string after = column(run_cypher(g, "MATCH (x {name:'a'})-[:KNOWS*1..4]->(y) RETURN y.name ORDER BY y.name"));
    CHECK(before == after && before == "a,b,b,c,d");

    bool threw = false;
    try { run_cypher(g, "MATCH (a)-[:KNOWS*]->(b) RETURN a"); } catch (const CypherError&) { threw = true; }
    CHECK(threw);
}

static void test_wal() {
    fs::path dir = fs::temp_directory_path() / ("gv_wal_test_" + std::to_string(getpid()));
    fs::remove_all(dir);
    {
        Store s(dir, false);
        Commit c = [&](const std::string& r) { return s.commit(r); };
        run_cypher(s.graph(), "CREATE (a:P {id: 1}), (b:P {id: 2})", {}, c);
        run_cypher(s.graph(), "MATCH (a {id: 1}), (b {id: 2}) CREATE (a)-[:R]->(b)", {}, c);
        CHECK(s.lsn() == 3);
        s.checkpoint();
        run_cypher(s.graph(), "CREATE (c:P {id: 3})", {}, c);
    }
    {
        Store s(dir, false);
        CHECK(s.lsn() == 4 && s.replayed() == 1);
        CHECK(s.graph().num_nodes() == 3 && s.graph().num_edges() == 1);
        CHECK(cell(run_cypher(s.graph(), "MATCH (a:P)-[:R]->(b) RETURN b.id"), 0, 0) == "2");
    }
    // Torn tail: half a record at the end is discarded, the rest survives.
    {
        std::ofstream f(dir / "wal.log", std::ios::app | std::ios::binary);
        f.write("\x40\x00\x00\x00garbage", 11);
    }
    {
        Store s(dir, false);
        CHECK(s.lsn() == 4 && s.discarded_bytes() == 11);
        CHECK(s.graph().num_nodes() == 3);
    }
    fs::remove_all(dir);
}

static void test_hnsw() {
    const size_t n = 4000, d = 32, nq = 100, k = 10;
    std::mt19937 rng(7);
    std::normal_distribution<float> nd;
    std::vector<float> data(n * d), qs(nq * d);
    for (auto& x : data) x = nd(rng);
    for (auto& x : qs) x = nd(rng);
    Hnsw h(d, n, {12, 100, 1});
    h.build(data.data(), n, 4);
    CHECK(h.size() == n);
    std::vector<uint8_t> allow(n);
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < n; ++i)
        if ((allow[i] = (i % 10 == 3))) ids.push_back(i);
    double rec = 0, rec_f = 0, rec_post = 0;
    for (size_t qi = 0; qi < nq; ++qi) {
        const float* q = &qs[qi * d];
        std::vector<uint32_t> all(n);
        for (uint32_t i = 0; i < n; ++i) all[i] = i;
        auto truth = prefilter_search(h, q, k, all);
        auto got = h.search(q, k, 100);
        std::set<uint32_t> t;
        for (auto& x : truth) t.insert(x.second);
        for (auto& x : got) rec += t.count(x.second);
        auto ftruth = prefilter_search(h, q, k, ids);
        std::set<uint32_t> ft;
        for (auto& x : ftruth) ft.insert(x.second);
        for (auto& x : inline_search(h, q, k, 100, allow)) rec_f += ft.count(x.second);
        for (auto& x : postfilter_search(h, q, k, 100, allow)) rec_post += ft.count(x.second);
    }
    rec /= double(nq * k);
    rec_f /= double(nq * k);
    rec_post /= double(nq * k);
    std::fprintf(stderr, "hnsw recall@10=%.3f inline(10%%)=%.3f postfilter(10%%)=%.3f\n", rec, rec_f, rec_post);
    CHECK(rec > 0.95);
    CHECK(rec_f > 0.95);
    CHECK(rec_post < rec_f);  // 100 candidates hold ~10 matches: post-filtering loses some
}

static void test_ring() {
    HashRing r(4, 64);
    std::vector<int> cnt(4);
    for (uint64_t k = 0; k < 40000; ++k) ++cnt[r.shard_of(k)];
    for (int c : cnt) CHECK(c > 6000 && c < 14000);
    HashRing r5(5, 64);
    int moved = 0;
    for (uint64_t k = 0; k < 40000; ++k) moved += r.shard_of(k) != r5.shard_of(k);
    CHECK(moved < 40000 / 5 * 1.5);  // ~1/5 of keys move, not most of them
}

int main() {
    test_cypher();
    test_wal();
    test_hnsw();
    test_ring();
    std::fprintf(stderr, "%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
