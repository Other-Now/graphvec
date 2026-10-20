// Property values: the dynamically typed scalars a Cypher query sees.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace gv {

struct Null {
    bool operator==(const Null&) const { return true; }
};

// List values only appear as query literals (`x IN [1, 2]`), never stored.
struct Value;
using List = std::vector<Value>;

struct Value {
    std::variant<Null, bool, int64_t, double, std::string, List> v;

    Value() = default;
    Value(Null) {}
    Value(bool b) : v(b) {}
    Value(int64_t i) : v(i) {}
    Value(int i) : v(int64_t{i}) {}
    Value(double d) : v(d) {}
    Value(std::string s) : v(std::move(s)) {}
    Value(const char* s) : v(std::string(s)) {}
    Value(List l) : v(std::move(l)) {}

    bool is_null() const { return std::holds_alternative<Null>(v); }
    bool is_int() const { return std::holds_alternative<int64_t>(v); }
    bool is_num() const { return is_int() || std::holds_alternative<double>(v); }
    bool is_bool() const { return std::holds_alternative<bool>(v); }
    bool is_str() const { return std::holds_alternative<std::string>(v); }
    bool is_list() const { return std::holds_alternative<List>(v); }
    int64_t as_int() const { return std::get<int64_t>(v); }
    double as_num() const { return is_int() ? double(std::get<int64_t>(v)) : std::get<double>(v); }
    bool as_bool() const { return std::get<bool>(v); }
    const std::string& as_str() const { return std::get<std::string>(v); }
    const List& as_list() const { return std::get<List>(v); }

    // Exact equality for hashing/grouping (1 and 1.0 are equal, as in Cypher).
    bool operator==(const Value& o) const;
    std::string to_string() const;
};

// Three-valued comparison: returns false in `ok` when the values are not
// comparable (null involved, or different types) -- Cypher yields null there.
int compare(const Value& a, const Value& b, bool& ok);

// Total order used by ORDER BY: nulls sort last ascending (Neo4j/Memgraph rule).
bool order_less(const Value& a, const Value& b);

struct ValueHash {
    size_t operator()(const Value& x) const;
};

} // namespace gv
