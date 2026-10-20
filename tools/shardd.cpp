// shardd: one replica of one shard. Two replicas per shard; etcd decides
// which one leads.
//
//   shardd --shard S --replica R --port P --peer HOST:PORT
//          --etcd 127.0.0.1:2379,... --data DIR [--ttl 2] [--unsafe]
//
// etcd keys
//   /gv/shard/S/leader  "R host:port", attached to the leader's lease. Its
//                       create_revision is the leader's FENCING TOKEN: every
//                       new leadership gets a strictly larger one.
//   /gv/shard/S/isr     in-sync replicas, e.g. "0,1". Only a replica in the
//                       ISR may become leader (it holds every acked write).
//
// Write path (leader): encode the WAL record -> send REPL to the backup and
// wait for ACK -> commit locally -> reply OK. If the backup is unreachable
// the leader first removes it from the ISR with a transaction guarded on its
// own token, so a leader that has been replaced cannot do it.
// Backups reject REPL carrying a token lower than one they have seen
// ("FENCED"), which stops a deposed leader from getting a write acked.
// Reads: the leader confirms with a linearizable etcd read that its token
// is still the current one before answering.
//
// --unsafe turns all of the fencing off (the control experiment): backups
// accept any token, reads skip the etcd check, ISR updates are unguarded.
//
// Client protocol (one line each way):
//   PUT k v -> OK | ERR NOTLEADER | ERR UNKNOWN      GET k -> VAL v | NIL | ERR ..
//   EDGE a b -> OK ..   (a's shard stores a -> b; b may live on another shard)
//   NBRS k -> NBRS k1 k2 ...     ROLE -> ROLE leader|follower token lsn
#include "graphvec/etcd.hpp"
#include "graphvec/net.hpp"
#include "graphvec/store.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace gv;

namespace {

struct Config {
    int shard = 0, replica = 0, ttl = 2;
    uint16_t port = 0;
    std::string peer, data;
    std::vector<std::string> etcd;
    bool unsafe = false;
};

enum Role { kFollower = 0, kLeader = 1 };

class Shard {
public:
    explicit Shard(const Config& c)
        : c_(c), store_(c.data, false), etcd_ctl_(c.etcd), etcd_data_(c.etcd),
          leader_key_("/gv/shard/" + std::to_string(c.shard) + "/leader"),
          isr_key_("/gv/shard/" + std::to_string(c.shard) + "/isr"),
          me_(std::to_string(c.replica) + " 127.0.0.1:" + std::to_string(c.port)) {
        ensure_index();
    }

    void run() {
        std::thread ctl([this] { control_loop(); });
        data_loop();
        ctl.join();
    }

private:
    // ---------------- helpers ----------------
    void log(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        std::fprintf(stderr, "%lld shard%d/r%d %s\n", (long long)(net::now_ns() / 1000000), c_.shard, c_.replica, buf);
    }

    void ensure_index() { store_.graph().create_index(store_.graph().keys.intern("key")); }

    NodeId lookup(int64_t key) {
        Graph& g = store_.graph();
        const auto& v = g.index_lookup(g.keys.intern("key"), Value(key));
        return v.empty() ? kNoNode : v[0];
    }

    static std::string isr_all() { return "0,1"; }
    std::string isr_me() const { return std::to_string(c_.replica); }
    static bool isr_has(const std::string& isr, int r) {
        std::stringstream ss(isr);
        for (std::string t; std::getline(ss, t, ',');)
            if (t == std::to_string(r)) return true;
        return false;
    }

    void step_down(const char* why) {
        if (role_.exchange(kFollower) == kLeader) log("step down (%s), token was %lld", why, (long long)token_.load());
        token_ = 0;
        synced_token_ = 0;
        std::lock_guard<std::mutex> g(peer_mu_);
        net::close_fd(peer_fd_);
    }

    // Is my token still the current leader key? (linearizable etcd read)
    bool confirm_leader() {
        Etcd::Kv kv;
        if (!etcd_data_.get(leader_key_, kv)) return false;
        return kv.found && kv.create_rev == token_.load();
    }

    // ---------------- control thread: leases and elections ----------------
    void control_loop() {
        int64_t last_ok = 0;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (role_ == kLeader) {
                int64_t now = net::now_ns();
                if (now - last_ka_ < int64_t(c_.ttl) * 1000000000 / 4) continue;
                last_ka_ = now;
                int64_t ttl = 0;
                if (etcd_ctl_.keepalive(lease_, ttl)) {
                    if (ttl <= 0) step_down("lease expired");
                    else last_ok = now;
                } else if (now - last_ok > int64_t(c_.ttl) * 1000000000) {
                    step_down("cannot reach etcd");
                }
                continue;
            }
            Etcd::Kv lk;
            if (!etcd_ctl_.get(leader_key_, lk)) continue;
            if (!lk.found) {
                campaign();
                last_ok = net::now_ns();
                continue;
            }
            int who = std::atoi(lk.value.c_str());
            if (who == c_.replica) continue;  // my previous incarnation's lease; wait for it to expire
            if (synced_token_ != lk.create_rev) sync_from(lk.value.substr(lk.value.find(' ') + 1), lk.create_rev);
        }
    }

    void campaign() {
        Etcd::Kv isr;
        if (!etcd_ctl_.get(isr_key_, isr)) return;
        if (!isr.found) {  // bootstrap: both empty replicas are in sync
            bool ok;
            int64_t rev;
            etcd_ctl_.txn({{Etcd::Cmp::Create, isr_key_, 0, ""}}, {{isr_key_, isr_all(), 0}}, ok, rev);
            return;
        }
        if (!isr_has(isr.value, c_.replica)) return;  // out of sync: not eligible
        int64_t lease = 0;
        if (!etcd_ctl_.grant(c_.ttl, lease)) return;
        bool won = false;
        int64_t rev = 0;
        if (!etcd_ctl_.txn({{Etcd::Cmp::Create, leader_key_, 0, ""}, {Etcd::Cmp::Mod, isr_key_, isr.mod_rev, ""}},
                           {{leader_key_, me_, lease}}, won, rev) ||
            !won) {
            etcd_ctl_.revoke(lease);
            return;
        }
        {
            std::lock_guard<std::mutex> g(peer_mu_);
            net::close_fd(peer_fd_);
            peer_in_isr_ = isr_has(isr.value, 1 - c_.replica);
        }
        lease_ = lease;
        token_ = rev;
        last_ka_ = net::now_ns();
        role_ = kLeader;
        log("LEADER token=%lld lsn=%llu isr=%s", (long long)rev, (unsigned long long)store_.lsn(), isr.value.c_str());
    }

    // Follower: copy the leader's whole state, then it adds us to the ISR.
    void sync_from(const std::string& addr, int64_t leader_token) {
        std::string host;
        uint16_t port;
        if (!net::split_hostport(addr, host, port)) return;
        int fd = net::connect_tcp(host, port, 300);
        if (fd < 0) return;
        net::LineReader rd(fd);
        std::string line;
        bool ok = false;
        if (net::send_all(fd, "SYNC " + std::to_string(c_.replica) + "\n") && rd.read_line(line, 5000) &&
            line.rfind("SNAP ", 0) == 0) {
            std::istringstream ss(line.substr(5));
            int64_t tok;
            unsigned long long lsn;
            std::string body;
            ss >> tok >> lsn >> body;
            {
                std::lock_guard<std::mutex> g(store_mu_);
                if (tok >= max_token_ || c_.unsafe) {
                    store_.install(net::unhex(body), lsn);
                    ensure_index();
                    max_token_ = std::max(max_token_, tok);
                    synced_token_ = tok;
                    ok = true;
                }
            }
            if (ok) ok = net::send_all(fd, "LOADED\n") && rd.read_line(line, 3000) && line == "DONE";
        }
        ::close(fd);
        if (ok) log("synced from leader token=%lld lsn=%llu", (long long)leader_token, (unsigned long long)store_.lsn());
        else synced_token_ = 0;
    }

    // ---------------- data thread ----------------
    struct Conn {
        int fd;
        net::LineReader rd;
    };

    void data_loop() {
        int lfd = net::listen_tcp(c_.port);
        if (lfd < 0) { log("cannot listen on %u", c_.port); std::exit(1); }
        log("listening, lsn=%llu (replayed %zu, discarded %zu bytes)", (unsigned long long)store_.lsn(),
            store_.replayed(), store_.discarded_bytes());
        std::vector<std::unique_ptr<Conn>> conns;
        while (true) {
            std::vector<pollfd> pf{{lfd, POLLIN, 0}};
            for (auto& c : conns) pf.push_back({c->fd, POLLIN, 0});
            if (poll(pf.data(), pf.size(), 200) <= 0) continue;
            if (pf[0].revents & POLLIN) {
                int fd = ::accept(lfd, nullptr, nullptr);
                if (fd >= 0) conns.push_back(std::make_unique<Conn>(Conn{fd, net::LineReader(fd)}));
            }
            for (size_t i = 1; i < pf.size(); ++i) {
                if (!pf[i].revents) continue;
                Conn& c = *conns[i - 1];
                bool alive = c.rd.fill();
                std::string line;
                while (alive && c.rd.next_buffered(line)) {
                    std::string reply = handle(line, c);
                    if (!reply.empty() && !net::send_all(c.fd, reply + "\n")) alive = false;
                }
                if (!alive) net::close_fd(c.fd);
            }
            std::erase_if(conns, [](auto& c) { return c->fd < 0; });
        }
    }

    std::string handle(const std::string& line, Conn& c) {
        std::istringstream ss(line);
        std::string op;
        ss >> op;
        if (op == "PUT") {
            int64_t k, v;
            ss >> k >> v;
            if (role_ != kLeader) return "ERR NOTLEADER";
            std::lock_guard<std::mutex> g(store_mu_);
            NodeId n = lookup(k);
            std::string r = n == kNoNode ? rec::create_node({"Item"}, {{"key", Value(k)}, {"v", Value(v)}})
                                         : rec::set_prop(n, "v", Value(v));
            return replicate_and_commit({r});
        }
        if (op == "EDGE") {
            int64_t a, b;
            ss >> a >> b;
            if (role_ != kLeader) return "ERR NOTLEADER";
            std::lock_guard<std::mutex> g(store_mu_);
            // Ensure both endpoints exist locally (b may be a stub for a node
            // owned by another shard), then add the edge. Node ids are
            // positional, so they are predicted from the current node count.
            std::vector<std::string> recs;
            NodeId na = lookup(a), nb = lookup(b);
            NodeId next = NodeId(store_.graph().num_nodes());
            if (na == kNoNode) { recs.push_back(rec::create_node({"Item"}, {{"key", Value(a)}})); na = next++; }
            if (nb == kNoNode) {
                if (b == a) nb = na;
                else { recs.push_back(rec::create_node({"Ref"}, {{"key", Value(b)}})); nb = next++; }
            }
            recs.push_back(rec::create_edge(na, nb, "LINK"));
            return replicate_and_commit(recs);
        }
        if (op == "GET" || op == "NBRS") {
            int64_t k;
            ss >> k;
            if (role_ != kLeader) return "ERR NOTLEADER";
            if (!c_.unsafe && !confirm_leader()) {
                step_down("read check: no longer leader");
                return "ERR NOTLEADER";
            }
            std::lock_guard<std::mutex> g(store_mu_);
            NodeId n = lookup(k);
            Graph& gr = store_.graph();
            if (op == "GET") {
                if (n == kNoNode) return "NIL";
                const Value& v = gr.prop(n, gr.keys.intern("v"));
                return v.is_null() ? "NIL" : "VAL " + v.to_string();
            }
            std::string out = "NBRS";
            if (n != kNoNode) {
                uint32_t kk = gr.keys.intern("key");
                gr.for_each_out(n, [&](const Adj& a) {
                    out += " " + gr.prop(a.nbr, kk).to_string();
                    return true;
                });
            }
            return out;
        }
        if (op == "REPL") return on_repl(ss);
        if (op == "SYNC") return on_sync(c);
        if (op == "ROLE") {
            return std::string("ROLE ") + (role_ == kLeader ? "leader " : "follower ") +
                   std::to_string(token_.load()) + " " + std::to_string(store_.lsn());
        }
        return "ERR BADREQUEST";
    }

    // Leader, holding store_mu_.
    std::string replicate_and_commit(const std::vector<std::string>& recs) {
        for (const std::string& r : recs) {
            bool peer;
            {
                std::lock_guard<std::mutex> g(peer_mu_);
                peer = peer_in_isr_;
            }
            if (peer) {
                std::string reply;
                bool sent = send_peer("REPL " + std::to_string(token_.load()) + " " +
                                          std::to_string(store_.lsn() + 1) + " " + net::hex(r),
                                      reply);
                if (sent && reply.rfind("ACK", 0) == 0) {
                    // replicated
                } else if (sent && reply.rfind("FENCED", 0) == 0 && !c_.unsafe) {
                    step_down("backup fenced us");
                    return "ERR NOTLEADER";  // the backup did not apply it
                } else {
                    // Backup unreachable or out of sync: drop it from the ISR,
                    // but only while we are still the leader.
                    bool ok = false;
                    int64_t rev;
                    if (c_.unsafe) ok = etcd_data_.put(isr_key_, isr_me());
                    else if (!etcd_data_.txn({{Etcd::Cmp::Create, leader_key_, token_.load(), ""}},
                                             {{isr_key_, isr_me(), 0}}, ok, rev))
                        ok = false;
                    if (!ok) {
                        step_down("ISR shrink refused");
                        return "ERR UNKNOWN";  // the backup may or may not have applied it
                    }
                    log("backup dropped from ISR (%s)", sent ? reply.c_str() : "no reply");
                    std::lock_guard<std::mutex> g(peer_mu_);
                    peer_in_isr_ = false;
                    net::close_fd(peer_fd_);
                }
            } else if (!c_.unsafe && !confirm_leader()) {
                step_down("write check: no longer leader");
                return "ERR NOTLEADER";
            }
            store_.commit(r);
        }
        return "OK";
    }

    bool send_peer(const std::string& msg, std::string& reply) {
        std::lock_guard<std::mutex> g(peer_mu_);
        if (peer_fd_ < 0) {
            std::string host;
            uint16_t port;
            net::split_hostport(c_.peer, host, port);
            peer_fd_ = net::connect_tcp(host, port, 200);
            if (peer_fd_ < 0) return false;
            peer_rd_.reset(peer_fd_);
        }
        if (!net::send_all(peer_fd_, msg + "\n") || !peer_rd_.read_line(reply, 300)) {
            net::close_fd(peer_fd_);
            return false;
        }
        return true;
    }

    // Backup side of REPL.
    std::string on_repl(std::istringstream& ss) {
        int64_t tok;
        unsigned long long lsn;
        std::string body;
        ss >> tok >> lsn >> body;
        std::lock_guard<std::mutex> g(store_mu_);
        if (!c_.unsafe) {
            int64_t mine = role_ == kLeader ? token_.load() : 0;
            if (tok < std::max(max_token_, mine)) return "FENCED " + std::to_string(std::max(max_token_, mine));
            max_token_ = std::max(max_token_, tok);
            if (tok != synced_token_) return "NEEDSYNC";
        }
        if (lsn != store_.lsn() + 1) return "NEEDSYNC";
        store_.commit(net::unhex(body));
        return "ACK " + std::to_string(lsn);
    }

    // Leader side of SYNC: ship a snapshot, wait for LOADED, re-admit to ISR.
    std::string on_sync(Conn& c) {
        if (role_ != kLeader) return "ERR NOTLEADER";
        std::lock_guard<std::mutex> g(store_mu_);
        std::string snap = "SNAP " + std::to_string(token_.load()) + " " + std::to_string(store_.lsn()) + " " +
                           net::hex(store_.graph().serialize()) + "\n";
        std::string line;
        if (!net::send_all(c.fd, snap) || !c.rd.read_line(line, 5000) || line != "LOADED") return "ERR SYNC";
        bool ok = false;
        int64_t rev;
        if (c_.unsafe) ok = etcd_data_.put(isr_key_, isr_all());
        else etcd_data_.txn({{Etcd::Cmp::Create, leader_key_, token_.load(), ""}}, {{isr_key_, isr_all(), 0}}, ok, rev);
        if (!ok) {
            step_down("ISR expand refused");
            return "ERR NOTLEADER";
        }
        {
            std::lock_guard<std::mutex> pg(peer_mu_);
            peer_in_isr_ = true;
            net::close_fd(peer_fd_);
        }
        log("backup re-synced at lsn=%llu, ISR=%s", (unsigned long long)store_.lsn(), isr_all().c_str());
        return "DONE";
    }

    Config c_;
    Store store_;
    std::mutex store_mu_;       // store_ + max_token_
    Etcd etcd_ctl_, etcd_data_; // one per thread
    std::string leader_key_, isr_key_, me_;
    std::atomic<int> role_{kFollower};
    std::atomic<int64_t> token_{0}, lease_{0}, synced_token_{0}, last_ka_{0};
    int64_t max_token_ = 0;
    std::mutex peer_mu_;  // peer_fd_/peer_rd_/peer_in_isr_
    int peer_fd_ = -1;
    net::LineReader peer_rd_;
    bool peer_in_isr_ = false;
};

} // namespace

int main(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&] { return std::string(argv[++i]); };
        if (a == "--shard") c.shard = std::stoi(val());
        else if (a == "--replica") c.replica = std::stoi(val());
        else if (a == "--port") c.port = uint16_t(std::stoi(val()));
        else if (a == "--peer") c.peer = val();
        else if (a == "--data") c.data = val();
        else if (a == "--ttl") c.ttl = std::stoi(val());
        else if (a == "--unsafe") c.unsafe = true;
        else if (a == "--etcd") {
            std::stringstream ss(val());
            for (std::string t; std::getline(ss, t, ',');) c.etcd.push_back(t);
        }
    }
    signal(SIGPIPE, SIG_IGN);
    Shard s(c);
    s.run();
}
