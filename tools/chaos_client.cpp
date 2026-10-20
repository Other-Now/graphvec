// chaos_client: drives the sharded cluster and records what it saw.
//
//   run     concurrent clients doing PUT/GET on a few hot keys (register
//           ops, checked for linearizability by checker/) plus write-once
//           PUTs of unique keys (checked for loss). Every op is written to
//           the history with wall-clock call/return times and an outcome:
//           ok, fail (definitely not applied: NOTLEADER) or info (unknown:
//           timeout, connection loss, ERR UNKNOWN).
//   verify  re-read every unique key whose PUT was acked; any missing or
//           wrong value is a lost acknowledged write.
//   khop    load a random graph with EDGE (each edge stored on its source's
//           shard), then run k-hop BFS through NBRS calls that fan out
//           across shards; compare with a local BFS over the same edges.
//
//   chaos_client run    --etcd E --shards 2 --clients 8 --duration 60 --history h.jsonl
//   chaos_client verify --etcd E --shards 2 --history h.jsonl
//   chaos_client khop   --etcd E --shards 2 [--keys 5000 --edges 20000 --hops 3]
//   chaos_client zombie --etcd E --pids PID_R0,PID_R1 [--rounds 20]
#include "graphvec/etcd.hpp"
#include "graphvec/net.hpp"
#include "graphvec/ring.hpp"

#include <signal.h>
#include <unistd.h>

#include <algorithm>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace gv;

namespace {

enum Outcome { kOk, kFail, kInfo };

// Per-thread connection set: one cached leader connection per shard.
class Router {
public:
    Router(std::vector<std::string> etcd, int shards) : etcd_(std::move(etcd), 500), ring_(shards, 64), conns_(shards) {}

    int shard_of(int64_t key) const { return ring_.shard_of(uint64_t(key)); }

    // Sends one request line to the shard's leader and reads one reply.
    Outcome call(int shard, const std::string& req, std::string& reply, int timeout_ms) {
        Conn& c = conns_[shard];
        if (c.fd < 0 && !connect(shard)) return kFail;  // nothing was sent
        if (!net::send_all(c.fd, req + "\n") || !c.rd.read_line(reply, timeout_ms)) {
            net::close_fd(c.fd);
            return kInfo;
        }
        if (reply == "ERR NOTLEADER") {
            net::close_fd(c.fd);
            return kFail;
        }
        if (reply.rfind("ERR", 0) == 0) {
            net::close_fd(c.fd);
            return kInfo;
        }
        return kOk;
    }

private:
    struct Conn {
        int fd = -1;
        net::LineReader rd;
    };
    bool connect(int shard) {
        Etcd::Kv kv;
        if (!etcd_.get("/gv/shard/" + std::to_string(shard) + "/leader", kv) || !kv.found) return false;
        std::string host;
        uint16_t port;
        if (!net::split_hostport(kv.value.substr(kv.value.find(' ') + 1), host, port)) return false;
        Conn& c = conns_[shard];
        c.fd = net::connect_tcp(host, port, 200);
        c.rd.reset(c.fd);
        return c.fd >= 0;
    }
    Etcd etcd_;
    HashRing ring_;
    std::vector<Conn> conns_;
};

struct Args {
    std::string mode, history = "history.jsonl";
    std::vector<std::string> etcd;
    int shards = 2, clients = 8, hot = 10, hops = 3;
    double duration = 30;
    int keys = 5000, edges = 20000;
};

int run(const Args& a) {
    std::ofstream out(a.history);
    std::mutex out_mu;
    std::atomic<bool> stop{false};
    std::atomic<long> counts[3] = {0, 0, 0};
    auto client = [&](int id) {
        Router r(a.etcd, a.shards);
        std::mt19937_64 rng(uint64_t(id) * 7919 + 1);
        int64_t seq = 0;
        std::string reply;
        while (!stop) {
            int dice = int(rng() % 100);
            bool unique = dice >= 85, get = dice < 40;
            int64_t key = unique ? 1000000 + int64_t(id) * 10000000 + seq : int64_t(rng() % uint64_t(a.hot));
            int64_t val = int64_t(id) * 1000000000LL + ++seq;
            int shard = r.shard_of(key);
            std::string req = get ? "GET " + std::to_string(key) : "PUT " + std::to_string(key) + " " + std::to_string(val);
            int64_t t0 = net::now_ns();
            Outcome o = r.call(shard, req, reply, 1000);
            int64_t t1 = net::now_ns();
            if (get && o == kOk) val = reply == "NIL" ? -1 : std::stoll(reply.substr(4));
            ++counts[o];
            char line[256];
            std::snprintf(line, sizeof line,
                          "{\"c\":%d,\"s\":%d,\"op\":\"%s\",\"k\":%lld,\"v\":%lld,\"call\":%lld,\"ret\":%lld,\"st\":\"%s\"}\n",
                          id, shard, get ? "get" : "put", (long long)key, (long long)val, (long long)t0,
                          (long long)t1, o == kOk ? "ok" : o == kFail ? "fail" : "info");
            {
                std::lock_guard<std::mutex> g(out_mu);
                out << line;
            }
            if (o != kOk) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };
    std::vector<std::thread> ts;
    for (int i = 0; i < a.clients; ++i) ts.emplace_back(client, i);
    std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(a.duration * 1000)));
    stop = true;
    for (auto& t : ts) t.join();
    std::printf("{\"ok\":%ld,\"fail\":%ld,\"info\":%ld}\n", counts[0].load(), counts[1].load(), counts[2].load());
    return 0;
}

int verify(const Args& a) {
    // Last acked value per unique key (each is written once).
    std::ifstream in(a.history);
    std::map<int64_t, int64_t> acked;
    std::string line;
    while (std::getline(in, line)) {
        long long c, s, k, v, call, ret;
        char op[8], st[8];
        if (std::sscanf(line.c_str(),
                        "{\"c\":%lld,\"s\":%lld,\"op\":\"%7[^\"]\",\"k\":%lld,\"v\":%lld,\"call\":%lld,\"ret\":%lld,\"st\":\"%7[^\"]\"",
                        &c, &s, op, &k, &v, &call, &ret, st) == 8 &&
            k >= 1000000 && std::string(op) == "put" && std::string(st) == "ok")
            acked[k] = v;
    }
    Router r(a.etcd, a.shards);
    long lost = 0, wrong = 0;
    std::string reply;
    for (auto& [k, v] : acked) {
        Outcome o;
        int tries = 0;
        while ((o = r.call(r.shard_of(k), "GET " + std::to_string(k), reply, 1000)) != kOk && tries++ < 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (o != kOk || reply == "NIL") ++lost;
        else if (std::stoll(reply.substr(4)) != v) ++wrong;
    }
    std::printf("{\"acked_unique_writes\":%zu,\"lost\":%ld,\"wrong_value\":%ld}\n", acked.size(), lost, wrong);
    return lost || wrong ? 1 : 0;
}

int khop(const Args& a) {
    std::mt19937_64 rng(11);
    std::unordered_map<int64_t, std::vector<int64_t>> adj;
    Router r(a.etcd, a.shards);
    std::string reply;
    std::set<std::pair<int64_t, int64_t>> seen;
    int64_t t0 = net::now_ns();
    while (int(seen.size()) < a.edges) {
        int64_t s = int64_t(rng() % uint64_t(a.keys)), d = int64_t(rng() % uint64_t(a.keys));
        if (s == d || !seen.insert({s, d}).second) continue;
        while (r.call(r.shard_of(s), "EDGE " + std::to_string(s) + " " + std::to_string(d), reply, 2000) != kOk)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        adj[s].push_back(d);
    }
    double load_s = double(net::now_ns() - t0) / 1e9;
    long mismatches = 0, cross = 0, calls = 0;
    double total_ms = 0;
    const int seeds = 50;
    for (int i = 0; i < seeds; ++i) {
        int64_t seed = int64_t(rng() % uint64_t(a.keys));
        // local truth
        std::set<int64_t> local{seed}, frontier{seed};
        for (int h = 0; h < a.hops; ++h) {
            std::set<int64_t> nx;
            for (int64_t u : frontier)
                for (int64_t v : adj[u])
                    if (local.insert(v).second) nx.insert(v);
            frontier = nx;
        }
        // distributed: one NBRS per frontier node, routed to its owner shard
        int64_t t = net::now_ns();
        std::set<int64_t> dist{seed}, fr{seed};
        for (int h = 0; h < a.hops; ++h) {
            std::set<int64_t> nx;
            for (int64_t u : fr) {
                int sh = r.shard_of(u);
                ++calls;
                if (sh != r.shard_of(seed)) ++cross;
                while (r.call(sh, "NBRS " + std::to_string(u), reply, 2000) != kOk)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                std::istringstream ss(reply.substr(4));
                for (int64_t v; ss >> v;)
                    if (dist.insert(v).second) nx.insert(v);
            }
            fr = nx;
        }
        total_ms += double(net::now_ns() - t) / 1e6;
        if (dist != local) ++mismatches;
    }
    std::printf("{\"edges\":%d,\"load_s\":%.2f,\"seeds\":%d,\"hops\":%d,\"mismatches\":%ld,\"nbrs_calls\":%ld,"
                "\"cross_shard_calls\":%ld,\"mean_query_ms\":%.2f}\n",
                a.edges, load_s, seeds, a.hops, mismatches, calls, cross, total_ms / seeds);
    return mismatches ? 1 : 0;
}

// Deterministic zombie-leader test (single shard, two replicas):
//   1. key = 1 (acked); open a direct connection to the leader L
//   2. SIGSTOP L until etcd shows the other replica F as leader
//   3. key = 2 through F (acked)
//   4. on the old connection send "GET key" and "PUT key 3" to L, SIGCONT L
//   5. a correct system answers both with NOTLEADER. A GET that returns 1
//      was invoked after 2 was acked: a stale read. A PUT that returns OK
//      and is gone once the cluster settles: a lost acknowledged write.
int zombie(const Args& a, const std::vector<int>& pids, int rounds) {
    Etcd etcd(a.etcd, 500);
    int stale = 0, zombie_acks = 0, lost = 0;
    std::vector<double> failover_ms;
    auto leader = [&](int& rep, std::string& addr) {
        Etcd::Kv kv;
        if (!etcd.get("/gv/shard/0/leader", kv) || !kv.found) return false;
        rep = std::atoi(kv.value.c_str());
        addr = kv.value.substr(kv.value.find(' ') + 1);
        return true;
    };
    // A fresh router per step: a cached connection to the paused leader
    // would leave a request queued in its socket that it serves first on
    // resume, ahead of the probe.
    auto must = [&](const std::string& req) {
        std::string reply;
        while (Router(a.etcd, 1).call(0, req, reply, 2000) != kOk)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return reply;
    };
    int skipped = 0;
    for (int round = 0; round < rounds; ++round) {
        int64_t key = 500 + round;
        // Only start once the previous zombie has re-synced and both replicas
        // are in the ISR again. Pausing the leader while it is the only ISR
        // member leaves no eligible replica, and the wait below never ends
        // (this hung a CI job until its timeout).
        Etcd::Kv isr;
        int64_t deadline = net::now_ns() + 20'000'000'000LL;
        while ((!etcd.get("/gv/shard/0/isr", isr) || isr.value != "0,1") && net::now_ns() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (isr.value != "0,1") {
            ++skipped;
            std::fprintf(stderr, "round %d: skipped, ISR stayed %s\n", round, isr.value.c_str());
            continue;
        }
        must("PUT " + std::to_string(key) + " 1");
        int L;
        std::string addr, host;
        uint16_t port;
        while (!leader(L, addr)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        net::split_hostport(addr, host, port);
        int fd = net::connect_tcp(host, port, 500);
        net::LineReader rd(fd);
        std::string reply;
        net::send_all(fd, "GET " + std::to_string(key) + "\n");
        rd.read_line(reply, 1000);  // warm: the connection reaches L's event loop

        int64_t t0 = net::now_ns();
        kill(pids[L], SIGSTOP);
        int now_leader = L;
        deadline = net::now_ns() + 15'000'000'000LL;
        while ((!leader(now_leader, addr) || now_leader == L) && net::now_ns() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (now_leader == L) {
            kill(pids[L], SIGCONT);
            ::close(fd);
            ++skipped;
            std::fprintf(stderr, "round %d: skipped, no new leader within 15 s\n", round);
            continue;
        }
        failover_ms.push_back(double(net::now_ns() - t0) / 1e6);
        must("PUT " + std::to_string(key) + " 2");

        net::send_all(fd, "GET " + std::to_string(key) + "\nPUT " + std::to_string(key) + " 3\n");
        kill(pids[L], SIGCONT);
        std::string g, p;
        bool got_g = rd.read_line(g, 3000), got_p = got_g && rd.read_line(p, 3000);
        ::close(fd);
        if (got_g && g == "VAL 1") ++stale;
        bool acked3 = got_p && p == "OK";
        zombie_acks += acked3;
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // let L step down and resync
        std::string fin = must("GET " + std::to_string(key));
        if (acked3 && fin != "VAL 3") ++lost;
        std::fprintf(stderr, "round %d: leader r%d paused, failover %.0f ms; zombie GET -> %s, PUT 3 -> %s, final %s\n",
                     round, L, failover_ms.back(), got_g ? g.c_str() : "(none)", got_p ? p.c_str() : "(none)",
                     fin.c_str());
    }
    std::sort(failover_ms.begin(), failover_ms.end());
    if (failover_ms.empty()) failover_ms.push_back(0);
    std::printf("{\"rounds\":%d,\"stale_reads\":%d,\"zombie_acked_writes\":%d,\"lost_acked_writes\":%d,"
                "\"skipped\":%d,\"failover_ms_p50\":%.0f,\"failover_ms_max\":%.0f}\n",
                rounds - skipped, stale, zombie_acks, lost, skipped, failover_ms[failover_ms.size() / 2],
                failover_ms.back());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    std::vector<int> pids;  // zombie: pid of replica 0, replica 1
    int rounds = 20;
    if (argc < 2) { std::fprintf(stderr, "usage: chaos_client run|verify|khop ...\n"); return 2; }
    a.mode = argv[1];
    if (a.mode == "now") {  // the fault log must use the history's clock
        std::printf("%lld\n", (long long)net::now_ns());
        return 0;
    }
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        auto val = [&] { return std::string(argv[++i]); };
        if (k == "--etcd") {
            std::stringstream ss(val());
            for (std::string t; std::getline(ss, t, ',');) a.etcd.push_back(t);
        } else if (k == "--shards") a.shards = std::stoi(val());
        else if (k == "--clients") a.clients = std::stoi(val());
        else if (k == "--duration") a.duration = std::stod(val());
        else if (k == "--history") a.history = val();
        else if (k == "--hot") a.hot = std::stoi(val());
        else if (k == "--keys") a.keys = std::stoi(val());
        else if (k == "--edges") a.edges = std::stoi(val());
        else if (k == "--hops") a.hops = std::stoi(val());
        else if (k == "--pids") {
            std::stringstream ss(val());
            for (std::string t; std::getline(ss, t, ',');) pids.push_back(std::stoi(t));
        } else if (k == "--rounds") rounds = std::stoi(val());
    }
    if (a.mode == "run") return run(a);
    if (a.mode == "verify") return verify(a);
    if (a.mode == "khop") return khop(a);
    if (a.mode == "zombie") return zombie(a, pids, rounds);
    return 2;
}
