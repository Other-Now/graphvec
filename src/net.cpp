#include "graphvec/net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>

namespace gv::net {

int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

int listen_tcp(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 128) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int connect_tcp(const std::string& host, uint16_t port, int timeout_ms) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) { ::close(fd); return -1; }
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    if (r != 0 && errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, timeout_ms) == 1) {
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            r = err == 0 ? 0 : -1;
        } else {
            r = -1;
        }
    }
    if (r != 0) { ::close(fd); return -1; }
    fcntl(fd, F_SETFL, fl);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

bool split_hostport(const std::string& hp, std::string& host, uint16_t& port) {
    auto c = hp.rfind(':');
    if (c == std::string::npos) return false;
    host = hp.substr(0, c);
    port = uint16_t(std::stoi(hp.substr(c + 1)));
    return true;
}

bool send_all(int fd, std::string_view d) {
    while (!d.empty()) {
        ssize_t n = ::send(fd, d.data(), d.size(), MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        d.remove_prefix(size_t(n));
    }
    return true;
}

void close_fd(int& fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

bool LineReader::next_buffered(std::string& out) {
    auto nl = buf_.find('\n');
    if (nl == std::string::npos) return false;
    out.assign(buf_, 0, nl);
    buf_.erase(0, nl + 1);
    return true;
}

bool LineReader::fill() {
    char tmp[65536];
    ssize_t n = ::recv(fd_, tmp, sizeof tmp, MSG_DONTWAIT);
    if (n > 0) { buf_.append(tmp, size_t(n)); return true; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return true;
    return false;
}

bool LineReader::read_line(std::string& out, int timeout_ms) {
    int64_t deadline = now_ns() + int64_t(timeout_ms) * 1000000;
    while (!next_buffered(out)) {
        int left = int((deadline - now_ns()) / 1000000);
        if (left <= 0 || fd_ < 0) return false;
        pollfd p{fd_, POLLIN, 0};
        int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        char tmp[65536];
        ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf_.append(tmp, size_t(n));
    }
    return true;
}

static bool read_all(int fd, std::string& out, int timeout_ms) {
    int64_t deadline = now_ns() + int64_t(timeout_ms) * 1000000;
    char tmp[16384];
    while (true) {
        int left = int((deadline - now_ns()) / 1000000);
        if (left <= 0) return false;
        pollfd p{fd, POLLIN, 0};
        int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n < 0) return false;
        if (n == 0) return true;
        out.append(tmp, size_t(n));
        // A complete non-chunked response can be detected early.
        auto he = out.find("\r\n\r\n");
        if (he != std::string::npos) {
            auto cl = out.find("Content-Length: ");
            if (cl != std::string::npos && cl < he) {
                size_t len = std::stoul(out.substr(cl + 16));
                if (out.size() >= he + 4 + len) return true;
            } else if (out.find("\r\n0\r\n\r\n", he) != std::string::npos) {
                return true;  // end of a chunked body
            }
        }
    }
}

bool http_post(const std::string& host, uint16_t port, const std::string& path, const std::string& body,
               int timeout_ms, int& status, std::string& response) {
    int fd = connect_tcp(host, port, timeout_ms);
    if (fd < 0) return false;
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: application/json\r\n" +
                      "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    std::string raw;
    bool ok = send_all(fd, req) && read_all(fd, raw, timeout_ms);
    ::close(fd);
    if (!ok) return false;
    auto he = raw.find("\r\n\r\n");
    if (raw.compare(0, 9, "HTTP/1.1 ") != 0 || he == std::string::npos) return false;
    status = std::atoi(raw.c_str() + 9);
    std::string headers = raw.substr(0, he);
    std::string b = raw.substr(he + 4);
    if (headers.find("Transfer-Encoding: chunked") != std::string::npos) {
        std::string dec;
        size_t p = 0;
        while (p < b.size()) {
            size_t e = b.find("\r\n", p);
            if (e == std::string::npos) break;
            size_t len = std::stoul(b.substr(p, e - p), nullptr, 16);
            if (len == 0) break;
            dec.append(b, e + 2, len);
            p = e + 2 + len + 2;
        }
        b = dec;
    }
    response = b;
    return true;
}

std::string hex(std::string_view s) {
    static const char* d = "0123456789abcdef";
    std::string o;
    o.reserve(s.size() * 2);
    for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 15]; }
    return o;
}

std::string unhex(std::string_view s) {
    auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::string o(s.size() / 2, '\0');
    for (size_t i = 0; i < o.size(); ++i) o[i] = char(v(s[2 * i]) << 4 | v(s[2 * i + 1]));
    return o;
}

} // namespace gv::net
