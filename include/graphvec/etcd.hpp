// Tiny etcd v3 client over the JSON gRPC-gateway (POST /v3/...), enough for
// leases, linearizable reads and compare-and-swap transactions. Tries each
// endpoint in turn, so it rides through the loss of an etcd member.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gv {

class Etcd {
public:
    explicit Etcd(std::vector<std::string> endpoints, int timeout_ms = 1000)
        : eps_(std::move(endpoints)), timeout_ms_(timeout_ms) {}

    struct Kv {
        bool found = false;
        std::string value;
        int64_t create_rev = 0, mod_rev = 0, lease = 0;
    };
    struct Cmp {
        enum Target { Create, Mod, Value } target;
        std::string key;
        int64_t rev = 0;    // Create/Mod
        std::string value;  // Value
    };
    struct Put {
        std::string key, value;
        int64_t lease = 0;
    };

    // All return false on a transport failure (no endpoint answered).
    bool get(const std::string& key, Kv& out);
    bool grant(int ttl_s, int64_t& lease);
    bool keepalive(int64_t lease, int64_t& ttl);  // ttl <= 0: the lease is gone
    bool revoke(int64_t lease);
    bool put(const std::string& key, const std::string& value, int64_t lease = 0);
    // If every compare holds, apply `puts`. `rev` is the store revision of
    // the transaction; a key created by it has create_revision == rev.
    bool txn(const std::vector<Cmp>& cmps, const std::vector<Put>& puts, bool& succeeded, int64_t& rev);

private:
    bool call(const std::string& path, const std::string& body, std::string& resp);
    std::vector<std::string> eps_;
    size_t cur_ = 0;
    int timeout_ms_;
};

} // namespace gv
