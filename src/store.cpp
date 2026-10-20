#include "graphvec/store.hpp"

#include "graphvec/codec.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace gv {
namespace fs = std::filesystem;

uint32_t crc32(std::string_view data) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

namespace rec {
enum Op : uint8_t { kNode = 1, kEdge = 2, kSet = 3, kIndex = 4 };

std::string create_node(const std::vector<std::string>& labels,
                        const std::vector<std::pair<std::string, Value>>& props) {
    Writer w;
    w.u8(kNode);
    w.u32(uint32_t(labels.size()));
    for (auto& l : labels) w.str(l);
    w.u32(uint32_t(props.size()));
    for (auto& [k, v] : props) { w.str(k); w.value(v); }
    return std::move(w.buf);
}
std::string create_edge(NodeId src, NodeId dst, std::string_view type) {
    Writer w;
    w.u8(kEdge); w.u32(src); w.u32(dst); w.str(type);
    return std::move(w.buf);
}
std::string set_prop(NodeId n, std::string_view key, const Value& v) {
    Writer w;
    w.u8(kSet); w.u32(n); w.str(key); w.value(v);
    return std::move(w.buf);
}
std::string create_index(std::string_view key) {
    Writer w;
    w.u8(kIndex); w.str(key);
    return std::move(w.buf);
}

uint32_t apply(Graph& g, std::string_view payload) {
    Reader r{payload};
    switch (r.u8()) {
    case kNode: {
        std::vector<uint32_t> ls(r.u32());
        for (auto& l : ls) l = g.labels.intern(r.str());
        Props ps(r.u32());
        for (auto& p : ps) { p.first = g.keys.intern(r.str()); p.second = r.value(); }
        return g.create_node(ls, std::move(ps));
    }
    case kEdge: {
        NodeId s = r.u32(), d = r.u32();
        return g.create_edge(s, d, g.types.intern(r.str()));
    }
    case kSet: {
        NodeId n = r.u32();
        uint32_t k = g.keys.intern(r.str());
        g.set_prop(n, k, r.value());
        return 0;
    }
    case kIndex: g.create_index(g.keys.intern(r.str())); return 0;
    default: throw std::runtime_error("unknown record op");
    }
}
} // namespace rec

void write_file_atomic(const fs::path& path, std::string_view data) {
    fs::path tmp = path;
    tmp += ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) throw std::runtime_error("open " + tmp.string());
    for (size_t off = 0; off < data.size();) {
        ssize_t w = ::write(fd, data.data() + off, data.size() - off);
        if (w < 0) { ::close(fd); throw std::runtime_error("write snapshot"); }
        off += size_t(w);
    }
    // Contents must be durable before the rename can expose them.
    if (::fsync(fd) != 0) { ::close(fd); throw std::runtime_error("fsync snapshot"); }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) throw std::runtime_error("rename snapshot");
    int dfd = ::open(path.parent_path().c_str(), O_RDONLY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
}

static std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Store::Store(fs::path dir, bool fsync_each) : dir_(std::move(dir)), fsync_each_(fsync_each) {
    fs::create_directories(dir_);
    recover();
}

Store::~Store() {
    if (fd_ >= 0) ::close(fd_);
}

void Store::open_log(bool truncate) {
    if (fd_ >= 0) ::close(fd_);
    int flags = O_WRONLY | O_CREAT | O_APPEND | (truncate ? O_TRUNC : 0);
    fd_ = ::open((dir_ / "wal.log").c_str(), flags, 0644);
    if (fd_ < 0) throw std::runtime_error("open wal");
}

void Store::recover() {
    g_ = Graph();
    lsn_ = 0;
    replayed_ = discarded_ = 0;
    // Snapshot: [u64 lsn][u32 crc][graph bytes].
    if (fs::exists(dir_ / "snapshot")) {
        std::string s = read_file(dir_ / "snapshot");
        Reader r{s};
        uint64_t lsn = r.u64();
        uint32_t crc = r.u32();
        std::string_view body = std::string_view(s).substr(12);
        if (crc32(body) != crc) throw std::runtime_error("snapshot checksum mismatch");
        g_.deserialize(body);
        lsn_ = lsn;
    }
    // Log: repeated [u32 len][u32 crc][u64 lsn][payload]. Stop at the first
    // short or corrupt record; that is a torn tail and is cut off.
    std::string log = fs::exists(dir_ / "wal.log") ? read_file(dir_ / "wal.log") : std::string();
    size_t p = 0, good = 0;
    while (p + 16 <= log.size()) {
        uint32_t len, crc;
        std::memcpy(&len, log.data() + p, 4);
        std::memcpy(&crc, log.data() + p + 4, 4);
        if (p + 8 + len > log.size() || len < 8) break;
        std::string_view body(log.data() + p + 8, len);
        if (crc32(body) != crc) break;
        uint64_t lsn;
        std::memcpy(&lsn, body.data(), 8);
        if (lsn > lsn_) {  // records <= snapshot LSN are already in the snapshot
            if (lsn != lsn_ + 1) throw std::runtime_error("wal gap");
            rec::apply(g_, body.substr(8));
            lsn_ = lsn;
            ++replayed_;
        }
        p += 8 + len;
        good = p;
    }
    discarded_ = log.size() - good;
    if (discarded_) fs::resize_file(dir_ / "wal.log", good);
    g_.compact();
    open_log(false);
}

uint32_t Store::commit(std::string_view payload) {
    // Apply before logging: apply only fails on a bad record (e.g. a missing
    // edge endpoint), so the log never holds a record that cannot replay.
    // The caller acks only after commit() returns, so a crash in between
    // loses nothing that was acknowledged.
    uint32_t r = rec::apply(g_, payload);
    Writer w;
    w.u64(lsn_ + 1);
    w.buf.append(payload);
    Writer h;
    h.u32(uint32_t(w.buf.size()));
    h.u32(crc32(w.buf));
    h.buf += w.buf;
    for (size_t off = 0; off < h.buf.size();) {
        ssize_t n = ::write(fd_, h.buf.data() + off, h.buf.size() - off);
        if (n < 0) throw std::runtime_error("wal write");
        off += size_t(n);
    }
    if (fsync_each_ && ::fdatasync(fd_) != 0) throw std::runtime_error("wal fsync");
    ++lsn_;
    if (g_.delta_edges() > 4096 && g_.delta_edges() * 8 > g_.num_edges()) g_.compact();
    return r;
}

void Store::checkpoint() {
    g_.compact();
    std::string body = g_.serialize();
    Writer w;
    w.u64(lsn_);
    w.u32(crc32(body));
    w.buf += body;
    write_file_atomic(dir_ / "snapshot", w.buf);
    open_log(true);  // a crash before this line replays records <= lsn as no-ops
}

void Store::install(std::string_view graph_bytes, uint64_t lsn) {
    g_.deserialize(graph_bytes);
    lsn_ = lsn;
    checkpoint();
}

void Store::reset() {
    std::error_code ec;
    fs::remove(dir_ / "snapshot", ec);
    open_log(true);
    g_ = Graph();
    lsn_ = 0;
}

} // namespace gv
