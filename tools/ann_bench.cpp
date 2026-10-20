// ann_bench: graphvec HNSW vs hnswlib on SIFT (TEXMEX .fvecs), same harness.
//   ann_bench --sift DIR --lib mine|hnswlib [--n 1000000] [--nq 10000] [--M 16]
//             [--efc 200] [--threads 4] [--ef 10,20,40,80,160,320] [--out f.csv]
//             [--perf-ctl FIFO]
// --perf-ctl writes enable/disable to a `perf stat|record --control fifo:FIFO`
// control pipe around the timed search loop, so counters cover search only.
// With --n below 1M the ground truth is recomputed by brute force over the
// subset (the shipped ivecs is for the full 1M base).
#include "graphvec/dataset.hpp"
#include "graphvec/hnsw.hpp"

#include <hnswlib/hnswlib.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>

using namespace gv;
using Clock = std::chrono::steady_clock;

static double rss_mb() {
    std::ifstream f("/proc/self/statm");
    long pages = 0, res = 0;
    f >> pages >> res;
    return double(res) * double(sysconf(_SC_PAGESIZE)) / 1048576.0;
}

static double secs(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

int main(int argc, char** argv) {
    std::string sift, lib = "mine", out, efs = "10,20,40,80,160,320", perf_ctl;
    size_t n = 1000000, nq = 10000;
    int M = 16, efc = 200, threads = 4;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&] { return std::string(argv[++i]); };
        if (a == "--sift") sift = val();
        else if (a == "--lib") lib = val();
        else if (a == "--n") n = std::stoul(val());
        else if (a == "--nq") nq = std::stoul(val());
        else if (a == "--M") M = std::stoi(val());
        else if (a == "--efc") efc = std::stoi(val());
        else if (a == "--threads") threads = std::stoi(val());
        else if (a == "--ef") efs = val();
        else if (a == "--out") out = val();
        else if (a == "--perf-ctl") perf_ctl = val();
    }
    std::FILE* ctl = perf_ctl.empty() ? nullptr : std::fopen(perf_ctl.c_str(), "w");
    auto perf = [&](const char* cmd) {
        if (ctl) { std::fputs(cmd, ctl); std::fflush(ctl); }
    };
    Matrix base = read_fvecs(sift + "/sift_base.fvecs", n);
    Matrix query = read_fvecs(sift + "/sift_query.fvecs", nq);
    n = base.rows;
    nq = query.rows;
    const size_t d = base.dim, k = 10;

    std::vector<std::vector<int32_t>> gt;
    if (n == 1000000) {
        gt = read_ivecs(sift + "/sift_groundtruth.ivecs", nq);
    } else {
        gt.resize(nq);
        std::atomic<size_t> next{0};
        auto work = [&] {
            for (size_t q; (q = next.fetch_add(1)) < nq;) {
                std::vector<std::pair<float, int32_t>> all(n);
                for (size_t i = 0; i < n; ++i) all[i] = {l2sq(query.row(q), base.row(i), d), int32_t(i)};
                std::partial_sort(all.begin(), all.begin() + long(k), all.end());
                for (size_t j = 0; j < k; ++j) gt[q].push_back(all[j].second);
            }
        };
        std::vector<std::thread> ts;
        for (int t = 0; t < threads; ++t) ts.emplace_back(work);
        for (auto& t : ts) t.join();
    }

    std::vector<int> ef_list;
    std::stringstream ss(efs);
    for (std::string tok; std::getline(ss, tok, ',');) ef_list.push_back(std::stoi(tok));

    double rss0 = rss_mb();
    auto t0 = Clock::now();
    std::unique_ptr<Hnsw> mine;
    std::unique_ptr<hnswlib::L2Space> space;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> theirs;
    if (lib == "mine") {
        mine = std::make_unique<Hnsw>(d, n, Hnsw::Params{M, efc, 100});
        mine->build(base.data.data(), n, threads);
    } else {
        space = std::make_unique<hnswlib::L2Space>(d);
        theirs = std::make_unique<hnswlib::HierarchicalNSW<float>>(space.get(), n, size_t(M), size_t(efc), 100);
        theirs->addPoint(base.row(0), 0);
        std::atomic<size_t> next{1};
        auto work = [&] {
            for (size_t i; (i = next.fetch_add(1)) < n;) theirs->addPoint(base.row(i), i);
        };
        std::vector<std::thread> ts;
        for (int t = 1; t < threads; ++t) ts.emplace_back(work);
        work();
        for (auto& t : ts) t.join();
    }
    double build_s = secs(t0);
    double mem = rss_mb() - rss0;
    std::fprintf(stderr, "%s n=%zu M=%d efc=%d threads=%d build=%.1fs index_rss=%.0fMB\n", lib.c_str(), n, M, efc,
                 threads, build_s, mem);

    std::FILE* f = out.empty() ? stdout : std::fopen(out.c_str(), "a");
    if (!out.empty() && std::ftell(f) == 0)
        std::fprintf(f, "lib,n,M,efc,threads,build_s,index_rss_mb,ef,recall10,qps,dist_per_query\n");
    for (int ef : ef_list) {
        if (mine) mine->reset_distance_count();
        else theirs->setEf(size_t(ef));
        std::vector<int32_t> res(nq * k, -1);
        perf("enable\n");
        auto t = Clock::now();
        for (size_t q = 0; q < nq; ++q) {
            int32_t* r = &res[q * k];
            if (mine) {
                for (auto& h : mine->search(query.row(q), k, size_t(ef))) *r++ = int32_t(h.second);
            } else {
                auto pq = theirs->searchKnn(query.row(q), k);
                for (; !pq.empty(); pq.pop()) *r++ = int32_t(pq.top().second);
            }
        }
        double el = secs(t);
        perf("disable\n");
        size_t hit = 0;  // recall is scored outside the timed loop
        for (size_t q = 0; q < nq; ++q) {
            std::set<int32_t> truth(gt[q].begin(), gt[q].begin() + long(k));
            for (size_t j = 0; j < k; ++j) hit += truth.count(res[q * k + j]);
        }
        // hnswlib's own counter only counts upper-layer hops, so it is not
        // comparable; leave the column empty for it.
        double dpq = mine ? double(mine->distance_count()) / double(nq) : 0;
        std::string dcol = mine ? std::to_string(int64_t(dpq)) : "";
        std::fprintf(f, "%s,%zu,%d,%d,%d,%.2f,%.0f,%d,%.4f,%.0f,%s\n", lib.c_str(), n, M, efc, threads, build_s,
                     mem, ef, double(hit) / double(nq * k), double(nq) / el, dcol.c_str());
        std::fflush(f);
    }
    if (f != stdout) std::fclose(f);
}
