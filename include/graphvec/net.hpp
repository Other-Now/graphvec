// Minimal blocking TCP helpers (Linux), line framing with timeouts, and an
// HTTP/1.1 POST used to talk to etcd's JSON gateway.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace gv::net {

// CLOCK_MONOTONIC: one system-wide clock shared by servers, clients and the
// chaos driver. Not CLOCK_REALTIME -- WSL2 steps the wall clock when it
// resyncs with the host, which once made an op "return" 0.54 s before it
// was called and turned every key in the history non-linearizable.
int64_t now_ns();

int listen_tcp(uint16_t port);  // 127.0.0.1:port, SO_REUSEADDR
int connect_tcp(const std::string& host, uint16_t port, int timeout_ms);  // -1 on failure
bool split_hostport(const std::string& hp, std::string& host, uint16_t& port);
bool send_all(int fd, std::string_view data);
void close_fd(int& fd);

// Buffered reader for newline-terminated messages on one socket.
class LineReader {
public:
    explicit LineReader(int fd = -1) : fd_(fd) {}
    void reset(int fd) { fd_ = fd; buf_.clear(); }
    int fd() const { return fd_; }
    // false on timeout, EOF or error
    bool read_line(std::string& out, int timeout_ms);
    // Non-blocking: pull whatever is readable now; false on EOF/error.
    bool fill();
    bool next_buffered(std::string& out);

private:
    int fd_;
    std::string buf_;
};

// POST body to http://host:port/path; returns false on transport failure.
bool http_post(const std::string& host, uint16_t port, const std::string& path, const std::string& body,
               int timeout_ms, int& status, std::string& response);

std::string hex(std::string_view s);
std::string unhex(std::string_view s);

} // namespace gv::net
