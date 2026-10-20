// Durable graph: every mutation is an encoded record appended to a
// write-ahead log before it is applied. checkpoint() writes a snapshot with
// the cdc-store commit pattern (temp file, fsync, rename, fsync dir) and then
// truncates the log; records carry their LSN so a crash between the rename
// and the truncate replays nothing twice.
#pragma once

#include "graphvec/graph.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gv {

// A mutation, encoded by name (not dictionary id) so a record is
// self-contained: it can be replayed into any graph, including a replica.
namespace rec {
std::string create_node(const std::vector<std::string>& labels,
                        const std::vector<std::pair<std::string, Value>>& props);
std::string create_edge(NodeId src, NodeId dst, std::string_view type);
std::string set_prop(NodeId n, std::string_view key, const Value& v);
std::string create_index(std::string_view key);
// Applies one record to g; returns the created node/edge id (or 0).
uint32_t apply(Graph& g, std::string_view payload);
} // namespace rec

class Store {
public:
    // fsync_each: fsync the log on every commit (power-loss durability).
    // Without it a commit survives kill -9 (the page cache outlives the
    // process) but not a power cut.
    Store(std::filesystem::path dir, bool fsync_each);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    Graph& graph() { return g_; }
    const Graph& graph() const { return g_; }
    uint64_t lsn() const { return lsn_; }

    // Log then apply. Returns what rec::apply returned.
    uint32_t commit(std::string_view payload);
    void checkpoint();
    // Drop everything (a replica about to receive a full copy).
    void reset();
    // Replace the whole state with a leader's snapshot, durably.
    void install(std::string_view graph_bytes, uint64_t lsn);
    // How many log records recovery replayed and how many trailing bytes it
    // discarded as a torn/corrupt tail.
    size_t replayed() const { return replayed_; }
    size_t discarded_bytes() const { return discarded_; }

private:
    void recover();
    void open_log(bool truncate);

    std::filesystem::path dir_;
    bool fsync_each_;
    Graph g_;
    uint64_t lsn_ = 0;
    int fd_ = -1;
    size_t replayed_ = 0, discarded_ = 0;
};

// Atomically replace `path` with `data` (temp + fsync + rename + fsync dir).
void write_file_atomic(const std::filesystem::path& path, std::string_view data);

} // namespace gv
