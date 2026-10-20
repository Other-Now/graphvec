// Little-endian binary encoding shared by snapshots, WAL records and the
// replication stream.
#pragma once

#include "graphvec/value.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gv {

struct Writer {
    std::string buf;
    void u8(uint8_t x) { buf.push_back(char(x)); }
    void u32(uint32_t x) { buf.append(reinterpret_cast<const char*>(&x), 4); }
    void u64(uint64_t x) { buf.append(reinterpret_cast<const char*>(&x), 8); }
    void f64(double x) { buf.append(reinterpret_cast<const char*>(&x), 8); }
    void str(std::string_view s) { u32(uint32_t(s.size())); buf.append(s); }
    void value(const Value& v) {
        if (v.is_null()) u8(0);
        else if (v.is_bool()) { u8(1); u8(v.as_bool()); }
        else if (v.is_int()) { u8(2); u64(uint64_t(v.as_int())); }
        else if (v.is_num()) { u8(3); f64(v.as_num()); }
        else if (v.is_str()) { u8(4); str(v.as_str()); }
        else throw std::runtime_error("lists cannot be stored");
    }
};

struct Reader {
    std::string_view s;
    size_t p = 0;
    void need(size_t n) const {
        if (p + n > s.size()) throw std::runtime_error("truncated record");
    }
    uint8_t u8() { need(1); return uint8_t(s[p++]); }
    uint32_t u32() { need(4); uint32_t x; std::memcpy(&x, s.data() + p, 4); p += 4; return x; }
    uint64_t u64() { need(8); uint64_t x; std::memcpy(&x, s.data() + p, 8); p += 8; return x; }
    double f64() { need(8); double x; std::memcpy(&x, s.data() + p, 8); p += 8; return x; }
    std::string str() { uint32_t n = u32(); need(n); std::string r(s.substr(p, n)); p += n; return r; }
    Value value() {
        switch (u8()) {
        case 0: return Value();
        case 1: return Value(bool(u8()));
        case 2: return Value(int64_t(u64()));
        case 3: return Value(f64());
        case 4: return Value(str());
        default: throw std::runtime_error("bad value tag");
        }
    }
    bool done() const { return p == s.size(); }
};

uint32_t crc32(std::string_view data);

} // namespace gv
