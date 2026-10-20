#include "graphvec/value.hpp"

#include <cmath>
#include <cstdio>

namespace gv {

bool Value::operator==(const Value& o) const {
    if (is_num() && o.is_num()) {
        if (is_int() && o.is_int()) return as_int() == o.as_int();
        return as_num() == o.as_num();
    }
    return v == o.v;
}

std::string Value::to_string() const {
    if (is_null()) return "null";
    if (is_bool()) return as_bool() ? "true" : "false";
    if (is_int()) return std::to_string(as_int());
    if (std::holds_alternative<double>(v)) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.17g", std::get<double>(v));
        return buf;
    }
    if (is_str()) return as_str();
    std::string s = "[";
    for (size_t i = 0; i < as_list().size(); ++i) s += (i ? ", " : "") + as_list()[i].to_string();
    return s + "]";
}

int compare(const Value& a, const Value& b, bool& ok) {
    ok = true;
    if (a.is_num() && b.is_num()) {
        if (a.is_int() && b.is_int()) return a.as_int() < b.as_int() ? -1 : a.as_int() > b.as_int();
        double x = a.as_num(), y = b.as_num();
        return x < y ? -1 : x > y;
    }
    if (a.is_str() && b.is_str()) return a.as_str().compare(b.as_str()) < 0 ? -1 : a.as_str() != b.as_str();
    if (a.is_bool() && b.is_bool()) return int(a.as_bool()) - int(b.as_bool());
    ok = false;
    return 0;
}

bool order_less(const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return !a.is_null() && b.is_null();
    bool ok;
    int c = compare(a, b, ok);
    if (ok) return c < 0;
    return a.v.index() < b.v.index();  // mixed types: order by type, deterministic
}

size_t ValueHash::operator()(const Value& x) const {
    if (x.is_int()) return std::hash<int64_t>{}(x.as_int());
    if (x.is_num()) {
        double d = x.as_num();
        if (d == std::floor(d) && std::fabs(d) < 9e18) return std::hash<int64_t>{}(int64_t(d));
        return std::hash<double>{}(d);
    }
    if (x.is_str()) return std::hash<std::string>{}(x.as_str());
    if (x.is_bool()) return x.as_bool() ? 0x9e37 : 0x7f4a;
    if (x.is_list()) {
        size_t h = 0xcbf29ce484222325ULL;
        for (const auto& e : x.as_list()) h = (h ^ (*this)(e)) * 0x100000001b3ULL;
        return h;
    }
    return 0;
}

} // namespace gv
