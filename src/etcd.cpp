#include "graphvec/etcd.hpp"

#include "graphvec/net.hpp"

#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>

namespace gv {
namespace {

// ---- just enough JSON to read etcd gateway responses ----
struct Json {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    std::string s;  // Str, and Num kept as text
    bool b = false;
    std::vector<Json> a;
    std::map<std::string, Json> o;

    const Json& operator[](const std::string& k) const {
        static const Json none;
        auto it = o.find(k);
        return it == o.end() ? none : it->second;
    }
    // etcd encodes int64 as strings; proto3 JSON omits zero values.
    int64_t i64() const { return (t == Str || t == Num) && !s.empty() ? std::stoll(s) : 0; }
};

struct JsonParser {
    const std::string& s;
    size_t p = 0;
    void ws() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
    Json parse() {
        ws();
        Json j;
        if (p >= s.size()) throw std::runtime_error("json: eof");
        char c = s[p];
        if (c == '{') {
            j.t = Json::Obj;
            ++p;
            ws();
            if (s[p] == '}') { ++p; return j; }
            while (true) {
                ws();
                std::string k = str();
                ws();
                ++p;  // ':'
                j.o[k] = parse();
                ws();
                if (s[p++] == '}') return j;
            }
        }
        if (c == '[') {
            j.t = Json::Arr;
            ++p;
            ws();
            if (s[p] == ']') { ++p; return j; }
            while (true) {
                j.a.push_back(parse());
                ws();
                if (s[p++] == ']') return j;
            }
        }
        if (c == '"') { j.t = Json::Str; j.s = str(); return j; }
        if (s.compare(p, 4, "true") == 0) { p += 4; j.t = Json::Bool; j.b = true; return j; }
        if (s.compare(p, 5, "false") == 0) { p += 5; j.t = Json::Bool; return j; }
        if (s.compare(p, 4, "null") == 0) { p += 4; return j; }
        j.t = Json::Num;
        size_t st = p;
        while (p < s.size() && (std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '-' || s[p] == '.' ||
                                s[p] == 'e' || s[p] == 'E' || s[p] == '+'))
            ++p;
        j.s = s.substr(st, p - st);
        return j;
    }
    std::string str() {
        std::string o;
        ++p;  // opening quote
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\') ++p;
            o += s[p++];
        }
        ++p;
        return o;
    }
};

Json parse_json(const std::string& s) {
    JsonParser jp{s};
    return jp.parse();
}

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64(const std::string& in) {
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(kB64[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(kB64[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string unb64(const std::string& in) {
    std::string out;
    int val = 0, bits = -8;
    for (char c : in) {
        const char* q = std::strchr(kB64, c);
        if (!q || c == '=' || c == '\0') break;
        val = (val << 6) + int(q - kB64);
        bits += 6;
        if (bits >= 0) {
            out.push_back(char((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// The gateway may answer in snake_case or camelCase depending on version.
int64_t field(const Json& j, const char* snake, const char* camel) {
    const Json& a = j[snake];
    return a.t != Json::Null ? a.i64() : j[camel].i64();
}

} // namespace

bool Etcd::call(const std::string& path, const std::string& body, std::string& resp) {
    for (size_t tries = 0; tries < eps_.size(); ++tries) {
        const std::string& ep = eps_[cur_];
        std::string host;
        uint16_t port;
        int status = 0;
        if (net::split_hostport(ep, host, port) &&
            net::http_post(host, port, path, body, timeout_ms_, status, resp) && status == 200)
            return true;
        cur_ = (cur_ + 1) % eps_.size();  // fail over to the next member
    }
    return false;
}

bool Etcd::get(const std::string& key, Kv& out) {
    std::string r;
    if (!call("/v3/kv/range", "{\"key\":\"" + b64(key) + "\"}", r)) return false;
    Json j = parse_json(r);
    out = Kv{};
    const Json& kvs = j["kvs"];
    if (kvs.t == Json::Arr && !kvs.a.empty()) {
        const Json& kv = kvs.a[0];
        out.found = true;
        out.value = unb64(kv["value"].s);
        out.create_rev = field(kv, "create_revision", "createRevision");
        out.mod_rev = field(kv, "mod_revision", "modRevision");
        out.lease = kv["lease"].i64();
    }
    return true;
}

bool Etcd::grant(int ttl_s, int64_t& lease) {
    std::string r;
    if (!call("/v3/lease/grant", "{\"TTL\":" + std::to_string(ttl_s) + "}", r)) return false;
    lease = parse_json(r)["ID"].i64();
    return lease != 0;
}

bool Etcd::keepalive(int64_t lease, int64_t& ttl) {
    std::string r;
    if (!call("/v3/lease/keepalive", "{\"ID\":\"" + std::to_string(lease) + "\"}", r)) return false;
    ttl = parse_json(r)["result"]["TTL"].i64();
    return true;
}

bool Etcd::revoke(int64_t lease) {
    std::string r;
    return call("/v3/lease/revoke", "{\"ID\":\"" + std::to_string(lease) + "\"}", r);
}

bool Etcd::put(const std::string& key, const std::string& value, int64_t lease) {
    std::string r;
    std::string body = "{\"key\":\"" + b64(key) + "\",\"value\":\"" + b64(value) + "\"";
    if (lease) body += ",\"lease\":\"" + std::to_string(lease) + "\"";
    return call("/v3/kv/put", body + "}", r);
}

bool Etcd::txn(const std::vector<Cmp>& cmps, const std::vector<Put>& puts, bool& succeeded, int64_t& rev) {
    std::string body = "{\"compare\":[";
    for (size_t i = 0; i < cmps.size(); ++i) {
        const Cmp& c = cmps[i];
        body += i ? "," : "";
        body += "{\"result\":\"EQUAL\",\"key\":\"" + b64(c.key) + "\",";
        if (c.target == Cmp::Create) body += "\"target\":\"CREATE\",\"create_revision\":\"" + std::to_string(c.rev) + "\"}";
        else if (c.target == Cmp::Mod) body += "\"target\":\"MOD\",\"mod_revision\":\"" + std::to_string(c.rev) + "\"}";
        else body += "\"target\":\"VALUE\",\"value\":\"" + b64(c.value) + "\"}";
    }
    body += "],\"success\":[";
    for (size_t i = 0; i < puts.size(); ++i) {
        const Put& p = puts[i];
        body += i ? "," : "";
        body += "{\"request_put\":{\"key\":\"" + b64(p.key) + "\",\"value\":\"" + b64(p.value) + "\"";
        if (p.lease) body += ",\"lease\":\"" + std::to_string(p.lease) + "\"";
        body += "}}";
    }
    body += "]}";
    std::string r;
    if (!call("/v3/kv/txn", body, r)) return false;
    Json j = parse_json(r);
    succeeded = j["succeeded"].b;
    rev = j["header"]["revision"].i64();
    return true;
}

} // namespace gv
