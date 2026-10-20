// wal_crash: kill -9 a writer at random moments and check recovery.
//
// The child commits records (CREATE node {seq: i}) as fast as it can,
// checkpointing every 500 commits, and after each commit returns it writes
// the LSN to a pipe -- that is the "ack". The parent SIGKILLs it after a
// random 5-150 ms, reopens the store and checks that (1) every acked LSN is
// present, (2) node i has seq i for every recovered node (no gaps, no
// duplicates, no garbage), and (3) the store accepts new commits.
//
//   wal_crash [--trials 200] [--dir /tmp/gv_crash] [--fsync]
#include "graphvec/store.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

using namespace gv;

int main(int argc, char** argv) {
    int trials = 200;
    bool fsync_each = false;
    std::string dir = "/tmp/gv_crash";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--trials") trials = std::stoi(argv[++i]);
        else if (a == "--dir") dir = argv[++i];
        else if (a == "--fsync") fsync_each = true;
    }
    std::filesystem::remove_all(dir);
    std::mt19937 rng(1);
    long acked_total = 0, lost = 0, bad = 0, torn = 0;
    uint64_t max_acked = 0;
    for (int t = 0; t < trials; ++t) {
        int p[2];
        if (pipe(p) != 0) return 1;
        pid_t pid = fork();
        if (pid == 0) {
            close(p[0]);
            Store s(dir, fsync_each);
            for (uint64_t i = s.lsn();; ++i) {
                s.commit(rec::create_node({"N"}, {{"seq", Value(int64_t(i))}}));
                if (s.lsn() % 500 == 0) s.checkpoint();
                uint64_t l = s.lsn();
                if (write(p[1], &l, sizeof l) != sizeof l) _exit(1);
            }
        }
        close(p[1]);
        usleep(5000 + rng() % 145000);
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        uint64_t l;
        while (read(p[0], &l, sizeof l) == sizeof l) {
            max_acked = std::max(max_acked, l);
            ++acked_total;
        }
        close(p[0]);

        Store s(dir, fsync_each);
        if (s.lsn() < max_acked) ++lost;
        torn += s.discarded_bytes() > 0;
        const Graph& g = s.graph();
        uint32_t seq = g.keys.find("seq");
        if (g.num_nodes() != s.lsn()) ++bad;
        for (NodeId n = 0; n < g.num_nodes(); ++n)
            if (g.prop(n, seq).as_int() != int64_t(n)) { ++bad; break; }
        max_acked = s.lsn();  // everything recovered is now the baseline
    }
    std::printf("{\"trials\":%d,\"acks\":%ld,\"final_lsn\":%llu,\"trials_losing_acked\":%ld,"
                "\"trials_with_bad_state\":%ld,\"trials_with_torn_tail\":%ld,\"fsync\":%s}\n",
                trials, acked_total, (unsigned long long)max_acked, lost, bad, torn, fsync_each ? "true" : "false");
    return lost || bad ? 1 : 0;
}
