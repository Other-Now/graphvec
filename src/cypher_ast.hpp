// Internal: Cypher AST produced by the parser and consumed by the planner.
#pragma once

#include "graphvec/value.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gv::cy {

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

struct Expr {
    enum Kind { Lit, Param, Var, Prop, Not, And, Or, Xor, Cmp, Arith, Neg, In, IsNull, IsNotNull, Func, ListLit };
    Kind kind;
    Value lit;
    std::string name;  // Var/Param name, Prop key, Func name (lower-case), Cmp/Arith operator
    std::vector<ExprPtr> kids;
    bool distinct = false;  // count(DISTINCT x)
    bool star = false;      // count(*)
    std::string text;       // source text, used to match ORDER BY to RETURN items

    // Filled in by the planner.
    int slot = -1;
    uint32_t key = UINT32_MAX;
};

struct NodePat {
    std::string var;  // planner names anonymous nodes "_anonN"
    std::vector<std::string> labels;
    std::vector<std::pair<std::string, ExprPtr>> props;
};

struct RelPat {
    enum Dir { Out, In, Both } dir = Out;
    std::vector<std::string> types;
    bool varlen = false;
    int min = 1, max = 1;
};

struct Pattern {
    std::vector<NodePat> nodes;  // nodes.size() == rels.size() + 1
    std::vector<RelPat> rels;
};

struct ReturnItem {
    ExprPtr e;
    std::string alias;
};

struct SortItem {
    ExprPtr e;
    bool desc = false;
};

struct Query {
    bool create_index = false;
    std::string index_label, index_key;

    std::vector<Pattern> match;
    ExprPtr where;
    std::vector<Pattern> create;
    bool has_return = false, distinct = false;
    std::vector<ReturnItem> ret;
    std::vector<SortItem> order;
    ExprPtr skip, limit;
};

Query parse(std::string_view src);

} // namespace gv::cy
