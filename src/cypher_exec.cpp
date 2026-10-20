// Planner and executor for the Cypher subset.
//
// Planning: each MATCH pattern becomes a start operator (index seek, label
// scan or all-nodes scan -- whichever the statistics say produces the fewest
// rows) followed by Expand operators walking outwards along the chain in
// both directions. WHERE is split into AND-conjuncts and each conjunct is
// pushed down to the first operator after which all of its variables are
// bound.
//
// Execution is push-based: operators are a vector of steps run by one
// recursive function, and finished rows are handed to the projection sink.
#include "cypher_ast.hpp"

#include "graphvec/cypher.hpp"
#include "graphvec/store.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace gv {
using namespace cy;

namespace {

using Row = std::vector<NodeId>;

struct VecHash {
    size_t operator()(const std::vector<Value>& v) const {
        size_t h = 0xcbf29ce484222325ULL;
        for (auto& x : v) h = (h ^ ValueHash{}(x)) * 0x100000001b3ULL;
        return h;
    }
};
struct VecEq {
    bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!(a[i] == b[i]) && !(a[i].is_null() && b[i].is_null())) return false;
        return true;
    }
};

bool is_agg_name(const std::string& n) {
    return n == "count" || n == "sum" || n == "min" || n == "max" || n == "avg";
}

// Bind-time checks for one variable: labels and inline {k: v} properties
// from every place the variable appears.
struct NodeCheck {
    std::vector<uint32_t> labels;
    bool impossible = false;  // names a label that does not exist
    std::vector<std::pair<uint32_t, ExprPtr>> props;
};

struct Step {
    enum Kind { Scan, Expand } kind = Scan;
    enum Src { All, Label, Index } src = All;
    int slot = -1;          // Scan: bound variable
    uint32_t label = 0;     // Scan by label
    uint32_t key = 0;       // Scan by index
    ExprPtr key_val;
    int from = -1, to = -1; // Expand
    RelPat::Dir dir = RelPat::Out;
    std::vector<uint32_t> types;
    bool type_impossible = false;
    bool varlen = false;
    int min = 1, max = 1;
    bool to_bound = false;
    const NodeCheck* check = nullptr;  // for the variable this step newly binds
    std::vector<ExprPtr> filters;
    double est = 0;
};

class Exec {
public:
    Exec(Graph& g, const Params& p, const Commit& c, const VectorHook* vec)
        : g_(g), params_(p), commit_(c), vec_(vec) {}

    Result run(Query& q) {
        if (q.create_index) {
            commit(rec::create_index(q.index_key));
            res_.plan = "CreateIndex(" + q.index_key + ")\n";
            return std::move(res_);
        }
        assign_slots(q);
        plan_match(q);
        if (!q.create.empty()) {
            run_create(q);
        } else if (q.has_return) {
            if (!try_vector_topk(q)) run_return(q);
        }
        return std::move(res_);
    }

private:
    // ================= planning =================
    int slot_of(const std::string& v) {
        auto it = slots_.find(v);
        if (it != slots_.end()) return it->second;
        int s = int(slots_.size());
        slots_.emplace(v, s);
        return s;
    }

    void assign_slots(Query& q) {
        int anon = 0;
        auto name_nodes = [&](std::vector<Pattern>& ps) {
            for (auto& p : ps)
                for (auto& n : p.nodes) {
                    if (n.var.empty()) n.var = "_anon" + std::to_string(anon++);
                    slot_of(n.var);
                }
        };
        name_nodes(q.match);
        match_vars_ = slots_.size();
        name_nodes(q.create);
        // Merge per-variable checks across all MATCH occurrences.
        for (auto& p : q.match)
            for (auto& n : p.nodes) {
                NodeCheck& c = checks_[n.var];
                for (auto& l : n.labels) {
                    uint32_t id = g_.labels.find(l);
                    if (id == UINT32_MAX) c.impossible = true;
                    else c.labels.push_back(id);
                }
                for (auto& [k, e] : n.props) {
                    resolve(e);
                    c.props.emplace_back(g_.keys.find(k), e);
                }
            }
        if (q.where) resolve(q.where);
        for (auto& r : q.ret) resolve(r.e);
        for (auto& o : q.order) resolve(o.e, true);
        for (auto& p : q.create)
            for (auto& n : p.nodes)
                for (auto& kv : n.props) resolve(kv.second);
    }

    // Resolve variable slots and property-key ids inside an expression.
    void resolve(const ExprPtr& e, bool allow_alias = false) {
        if (!e) return;
        if (e->kind == Expr::Var) {
            auto it = slots_.find(e->name);
            if (it != slots_.end()) e->slot = it->second;
            else if (!allow_alias) throw CypherError("unknown variable " + e->name);
        }
        if (e->kind == Expr::Prop) e->key = g_.keys.find(e->name);
        for (auto& k : e->kids) resolve(k, allow_alias);
    }

    static void vars_of(const ExprPtr& e, std::set<int>& out) {
        if (e->kind == Expr::Var && e->slot >= 0) out.insert(e->slot);
        for (auto& k : e->kids) vars_of(k, out);
    }

    static void split_and(const ExprPtr& e, std::vector<ExprPtr>& out) {
        if (!e) return;
        if (e->kind == Expr::And) {
            split_and(e->kids[0], out);
            split_and(e->kids[1], out);
        } else {
            out.push_back(e);
        }
    }

    // Is `e` of the form var.key = <constant>? Returns the constant side.
    static ExprPtr eq_const(const ExprPtr& e, int slot, uint32_t& key) {
        if (e->kind != Expr::Cmp || e->name != "=") return nullptr;
        for (int side = 0; side < 2; ++side) {
            const ExprPtr& p = e->kids[side];
            const ExprPtr& c = e->kids[1 - side];
            std::set<int> vs;
            vars_of(c, vs);
            if (p->kind == Expr::Prop && p->kids[0]->kind == Expr::Var && p->kids[0]->slot == slot &&
                vs.empty() && !has_agg(c)) {
                key = p->key;
                return c;
            }
        }
        return nullptr;
    }

    static bool has_agg(const ExprPtr& e) {
        if (e->kind == Expr::Func && is_agg_name(e->name)) return true;
        for (auto& k : e->kids)
            if (has_agg(k)) return true;
        return false;
    }

    // Cheapest way to produce candidates for variable `var`.
    Step best_start(const std::string& var, const std::vector<ExprPtr>& conj) {
        Step s;
        s.kind = Step::Scan;
        s.slot = slots_.at(var);
        s.src = Step::All;
        s.est = double(g_.num_nodes());
        const NodeCheck& c = checks_[var];
        if (c.impossible) { s.est = 0; return s; }
        for (uint32_t l : c.labels) {
            double n = double(g_.nodes_with_label(l).size());
            if (n < s.est) { s.src = Step::Label; s.label = l; s.est = n; }
        }
        auto consider_index = [&](uint32_t key, const ExprPtr& val) {
            if (key == UINT32_MAX || !g_.indexed(key)) return;
            double n = double(g_.index_lookup(key, eval(val, {})).size());
            if (n <= s.est) { s.src = Step::Index; s.key = key; s.key_val = val; s.est = n; }
        };
        for (auto& [k, e] : c.props) {
            std::set<int> vs;
            vars_of(e, vs);
            if (vs.empty()) consider_index(k, e);
        }
        for (auto& e : conj) {
            uint32_t key;
            if (ExprPtr val = eq_const(e, s.slot, key)) consider_index(key, val);
        }
        return s;
    }

    void plan_match(Query& q) {
        std::vector<ExprPtr> conj;
        split_and(q.where, conj);
        std::vector<bool> bound(slots_.size(), false);
        double rows = 1;
        for (auto& p : q.match) {
            // Start from an already-bound variable if the pattern shares one,
            // else from the most selective node.
            size_t start = SIZE_MAX;
            for (size_t i = 0; i < p.nodes.size() && start == SIZE_MAX; ++i)
                if (bound[slots_[p.nodes[i].var]]) start = i;
            if (start == SIZE_MAX) {
                double best = 1e300;
                Step bs;
                for (size_t i = 0; i < p.nodes.size(); ++i) {
                    Step s = best_start(p.nodes[i].var, conj);
                    if (s.est < best) { best = s.est; bs = s; start = i; }
                }
                bs.check = &checks_[p.nodes[start].var];
                bound[bs.slot] = true;
                rows *= std::max(1.0, bs.est);
                bs.est = rows;
                steps_.push_back(bs);
            }
            auto expand = [&](size_t a, size_t b, const RelPat& r, bool reverse) {
                Step s;
                s.kind = Step::Expand;
                s.from = slots_[p.nodes[a].var];
                s.to = slots_[p.nodes[b].var];
                s.dir = r.dir == RelPat::Both ? RelPat::Both
                        : reverse              ? (r.dir == RelPat::Out ? RelPat::In : RelPat::Out)
                                               : r.dir;
                for (auto& t : r.types) {
                    uint32_t id = g_.types.find(t);
                    if (id != UINT32_MAX) s.types.push_back(id);
                }
                s.type_impossible = !r.types.empty() && s.types.empty();
                s.varlen = r.varlen;
                s.min = r.min;
                s.max = r.max;
                s.to_bound = bound[s.to];
                if (!s.to_bound) s.check = &checks_[p.nodes[b].var];
                bound[s.to] = true;
                double fan = g_.num_nodes() ? double(g_.num_edges()) / double(g_.num_nodes()) : 0;
                rows *= s.to_bound ? 1.0 : std::pow(std::max(1.0, fan), std::max(1, s.max));
                s.est = rows;
                steps_.push_back(s);
            };
            for (size_t j = start; j + 1 < p.nodes.size(); ++j) expand(j, j + 1, p.rels[j], false);
            for (size_t j = start; j-- > 0;) expand(j + 1, j, p.rels[j], true);
        }
        // Push each conjunct down to the first step that binds all its vars.
        std::vector<int> bound_at(slots_.size(), -1);
        for (size_t i = 0; i < steps_.size(); ++i) {
            int s = steps_[i].kind == Step::Scan ? steps_[i].slot : steps_[i].to;
            if (bound_at[s] < 0) bound_at[s] = int(i);
        }
        for (auto& e : conj) {
            if (has_agg(e)) throw CypherError("aggregate in WHERE");
            std::set<int> vs;
            vars_of(e, vs);
            int at = 0;
            for (int v : vs) {
                if (v >= int(match_vars_) || bound_at[v] < 0) throw CypherError("WHERE uses an unbound variable");
                at = std::max(at, bound_at[v]);
            }
            if (steps_.empty()) throw CypherError("WHERE without MATCH");
            steps_[at].filters.push_back(e);
        }
        for (auto& s : steps_) res_.plan += describe(s) + "\n";
    }

    std::string slot_name(int s) const {
        for (auto& [k, v] : slots_)
            if (v == s) return k;
        return "?";
    }

    std::string describe(const Step& s) const {
        std::string d;
        if (s.kind == Step::Scan) {
            if (s.src == Step::Index) d = "NodeIndexSeek(" + slot_name(s.slot) + "." + g_.keys.name(s.key) + ")";
            else if (s.src == Step::Label) d = "NodeByLabelScan(" + slot_name(s.slot) + ":" + g_.labels.name(s.label) + ")";
            else d = "AllNodesScan(" + slot_name(s.slot) + ")";
        } else {
            std::string rel = "[";
            for (size_t i = 0; i < s.types.size(); ++i) rel += (i ? "|:" : ":") + g_.types.name(s.types[i]);
            if (s.varlen) rel += "*" + std::to_string(s.min) + ".." + std::to_string(s.max);
            rel += "]";
            d = std::string(s.varlen ? "VarExpand" : "Expand") + (s.to_bound ? "Into" : "") + "(" +
                slot_name(s.from) + ")" + (s.dir == RelPat::In ? "<-" : "-") + rel +
                (s.dir == RelPat::Out ? "->" : "-") + "(" + slot_name(s.to) + ")";
        }
        char buf[64];
        std::snprintf(buf, sizeof buf, " est_rows=%.0f", s.est);
        d += buf;
        for (auto& f : s.filters) d += " filter[" + f->text + "]";
        return d;
    }

    // ================= expressions =================
    Value eval(const ExprPtr& e, const Row& row) const {
        switch (e->kind) {
        case Expr::Lit: return e->lit;
        case Expr::Param: {
            auto it = params_.find(e->name);
            if (it == params_.end()) throw CypherError("missing parameter $" + e->name);
            return it->second;
        }
        case Expr::Var:
            if (e->slot < 0 || size_t(e->slot) >= row.size() || row[e->slot] == kNoNode) return Value();
            return Value(int64_t(row[e->slot]));
        case Expr::Prop: {
            const ExprPtr& base = e->kids[0];
            if (base->kind != Expr::Var) return Value();
            if (base->slot < 0 || size_t(base->slot) >= row.size() || row[base->slot] == kNoNode) return Value();
            if (e->key == UINT32_MAX) return Value();
            return g_.prop(row[base->slot], e->key);
        }
        case Expr::Not: {
            Value v = eval(e->kids[0], row);
            return v.is_bool() ? Value(!v.as_bool()) : Value();
        }
        case Expr::And: case Expr::Or: case Expr::Xor: {
            Value a = eval(e->kids[0], row);
            if (e->kind == Expr::And && a.is_bool() && !a.as_bool()) return Value(false);
            if (e->kind == Expr::Or && a.is_bool() && a.as_bool()) return Value(true);
            Value b = eval(e->kids[1], row);
            if (e->kind == Expr::And) {
                if (b.is_bool() && !b.as_bool()) return Value(false);
                if (a.is_bool() && b.is_bool()) return Value(true);
                return Value();
            }
            if (e->kind == Expr::Or) {
                if (b.is_bool() && b.as_bool()) return Value(true);
                if (a.is_bool() && b.is_bool()) return Value(false);
                return Value();
            }
            if (a.is_bool() && b.is_bool()) return Value(a.as_bool() != b.as_bool());
            return Value();
        }
        case Expr::Cmp: {
            Value a = eval(e->kids[0], row), b = eval(e->kids[1], row);
            if (a.is_null() || b.is_null()) return Value();
            const std::string& op = e->name;
            if (op == "=" || op == "<>") {
                bool ok;
                compare(a, b, ok);
                bool eq = ok ? a == b : false;  // different types are simply unequal
                return Value(op == "=" ? eq : !eq);
            }
            bool ok;
            int c = compare(a, b, ok);
            if (!ok) return Value();
            if (op == "<") return Value(c < 0);
            if (op == "<=") return Value(c <= 0);
            if (op == ">") return Value(c > 0);
            return Value(c >= 0);
        }
        case Expr::Arith: {
            Value a = eval(e->kids[0], row), b = eval(e->kids[1], row);
            if (a.is_null() || b.is_null()) return Value();
            const std::string& op = e->name;
            if (op == "+" && a.is_str() && b.is_str()) return Value(a.as_str() + b.as_str());
            if (!a.is_num() || !b.is_num()) throw CypherError("arithmetic on non-numbers: " + e->text);
            if (a.is_int() && b.is_int()) {
                int64_t x = a.as_int(), y = b.as_int();
                if (op == "+") return Value(x + y);
                if (op == "-") return Value(x - y);
                if (op == "*") return Value(x * y);
                if (y == 0) throw CypherError("division by zero");
                return op == "/" ? Value(x / y) : Value(x % y);
            }
            double x = a.as_num(), y = b.as_num();
            if (op == "+") return Value(x + y);
            if (op == "-") return Value(x - y);
            if (op == "*") return Value(x * y);
            return op == "/" ? Value(x / y) : Value(std::fmod(x, y));
        }
        case Expr::Neg: {
            Value a = eval(e->kids[0], row);
            if (a.is_int()) return Value(-a.as_int());
            if (a.is_num()) return Value(-a.as_num());
            return Value();
        }
        case Expr::In: {
            Value x = eval(e->kids[0], row), l = eval(e->kids[1], row);
            if (l.is_null()) return Value();
            if (!l.is_list()) throw CypherError("IN expects a list");
            if (x.is_null()) return l.as_list().empty() ? Value(false) : Value();
            bool saw_null = false;
            for (auto& v : l.as_list()) {
                if (v.is_null()) { saw_null = true; continue; }
                bool ok;
                compare(x, v, ok);
                if (ok && x == v) return Value(true);
            }
            return saw_null ? Value() : Value(false);
        }
        case Expr::IsNull: return Value(eval(e->kids[0], row).is_null());
        case Expr::IsNotNull: return Value(!eval(e->kids[0], row).is_null());
        case Expr::ListLit: {
            List l;
            for (auto& k : e->kids) l.push_back(eval(k, row));
            return Value(std::move(l));
        }
        case Expr::Func: return call(e, row);
        }
        return Value();
    }

    Value call(const ExprPtr& e, const Row& row) const {
        const std::string& f = e->name;
        if (is_agg_name(f)) throw CypherError("aggregate " + f + "() used outside RETURN");
        auto arg = [&](size_t i) {
            if (i >= e->kids.size()) throw CypherError(f + "() needs more arguments");
            return eval(e->kids[i], row);
        };
        if (f == "id") return arg(0);
        if (f == "abs") {
            Value v = arg(0);
            return v.is_int() ? Value(int64_t(std::llabs(v.as_int()))) : v.is_num() ? Value(std::fabs(v.as_num())) : Value();
        }
        if (f == "tointeger") {
            Value v = arg(0);
            return v.is_num() ? Value(int64_t(v.as_num())) : Value();
        }
        if (f == "tofloat") {
            Value v = arg(0);
            return v.is_num() ? Value(v.as_num()) : Value();
        }
        if (f == "coalesce") {
            for (size_t i = 0; i < e->kids.size(); ++i) {
                Value v = arg(i);
                if (!v.is_null()) return v;
            }
            return Value();
        }
        if (f == "size") {
            Value v = arg(0);
            if (v.is_list()) return Value(int64_t(v.as_list().size()));
            if (v.is_str()) return Value(int64_t(v.as_str().size()));
            return Value();
        }
        if (f == "vec_dist") {
            if (!vec_) throw CypherError("vec_dist() needs a vector index");
            Value n = arg(0), q = arg(1);
            if (n.is_null()) return Value();
            return Value(double(vec_->dist(NodeId(n.as_int()), to_floats(q))));
        }
        throw CypherError("unknown function " + f + "()");
    }

    static std::vector<float> to_floats(const Value& v) {
        if (!v.is_list()) throw CypherError("vector parameter must be a list of numbers");
        std::vector<float> out;
        for (auto& x : v.as_list()) out.push_back(float(x.as_num()));
        return out;
    }

    bool truthy(const ExprPtr& e, const Row& row) const {
        Value v = eval(e, row);
        return v.is_bool() && v.as_bool();
    }

    // ================= matching =================
    bool node_ok(const Step& s, NodeId n, const Row& row) const {
        if (s.check) {
            for (uint32_t l : s.check->labels)
                if (!g_.has_label(n, l)) return false;
            for (auto& [k, e] : s.check->props) {
                if (k == UINT32_MAX) return false;
                const Value& have = g_.prop(n, k);
                Value want = eval(e, row);
                bool ok;
                compare(have, want, ok);
                if (have.is_null() || want.is_null() || !ok || !(have == want)) return false;
            }
        }
        for (auto& f : s.filters)
            if (!truthy(f, row)) return false;
        return true;
    }

    template <class Sink>
    void match(size_t i, Row& row, Sink& sink) {
        if (stop_) return;
        if (i == steps_.size()) { sink(row); return; }
        const Step& s = steps_[i];
        if (s.kind == Step::Scan) {
            if (s.check && s.check->impossible) return;
            auto visit = [&](NodeId n) {
                row[s.slot] = n;
                if (node_ok(s, n, row)) match(i + 1, row, sink);
                row[s.slot] = kNoNode;
                return !stop_;
            };
            if (s.src == Step::Index) {
                for (NodeId n : g_.index_lookup(s.key, eval(s.key_val, row)))
                    if (!visit(n)) break;
            } else if (s.src == Step::Label) {
                for (NodeId n : g_.nodes_with_label(s.label))
                    if (!visit(n)) break;
            } else {
                for (NodeId n = 0; n < g_.num_nodes(); ++n)
                    if (!visit(n)) break;
            }
            return;
        }
        if (s.type_impossible || (s.check && s.check->impossible)) return;
        expand(s, i, row[s.from], 0, row, sink);
    }

    bool type_ok(const Step& s, uint32_t t) const {
        if (s.types.empty()) return true;
        return std::find(s.types.begin(), s.types.end(), t) != s.types.end();
    }

    // Walk `depth` hops from u; emits a binding for every path whose length
    // is in [min, max]. Relationship uniqueness: an edge already used
    // anywhere in this MATCH (this path or another pattern part) is skipped.
    template <class Sink>
    void expand(const Step& s, size_t i, NodeId u, int depth, Row& row, Sink& sink) {
        if (stop_) return;
        int lo = s.varlen ? s.min : 1, hi = s.varlen ? s.max : 1;
        if (depth >= lo) {
            if (s.to_bound) {
                if (row[s.to] == u) match(i + 1, row, sink);
            } else {
                row[s.to] = u;
                if (node_ok(s, u, row)) match(i + 1, row, sink);
                row[s.to] = kNoNode;
            }
        }
        if (depth == hi) return;
        auto step = [&](const Adj& a) {
            if (!type_ok(s, a.type)) return true;
            if (std::find(used_.begin(), used_.end(), a.eid) != used_.end()) return true;
            used_.push_back(a.eid);
            expand(s, i, a.nbr, depth + 1, row, sink);
            used_.pop_back();
            return !stop_;
        };
        if (s.dir != RelPat::In && !g_.for_each_out(u, step)) return;
        if (s.dir != RelPat::Out) g_.for_each_in(u, step);
    }

    // ================= RETURN =================
    struct Agg {
        int64_t count = 0;
        bool sum_float = false;
        int64_t isum = 0;
        double fsum = 0;
        Value best;
        std::unordered_set<Value, ValueHash> seen;
    };

    int64_t eval_count(const ExprPtr& e) const {
        if (!e) return -1;
        Value v = eval(e, {});
        if (!v.is_int() || v.as_int() < 0) throw CypherError("SKIP/LIMIT must be a non-negative integer");
        return v.as_int();
    }

    // Map an ORDER BY item onto a RETURN column; -1 if it is not one.
    int order_column(const Query& q, const ExprPtr& e) const {
        for (size_t c = 0; c < q.ret.size(); ++c) {
            if (e->kind == Expr::Var && e->slot < 0 && e->name == q.ret[c].alias) return int(c);
            if (e->text == q.ret[c].alias || e->text == q.ret[c].e->text) return int(c);
        }
        return -1;
    }

    void run_return(const Query& q) {
        std::vector<bool> is_agg;
        bool aggregating = false;
        for (auto& r : q.ret) {
            bool a = r.e->kind == Expr::Func && is_agg_name(r.e->name);
            for (auto& k : r.e->kids)
                if (has_agg(k)) throw CypherError("nested aggregates are not supported");
            if (!a && has_agg(r.e)) throw CypherError("aggregate inside an expression is not supported");
            is_agg.push_back(a);
            aggregating |= a;
            res_.columns.push_back(r.alias);
        }
        std::vector<int> ocol;
        for (auto& o : q.order) {
            int c = order_column(q, o.e);
            if (c < 0 && (aggregating || q.distinct))
                throw CypherError("ORDER BY must name a RETURN column in an aggregating/DISTINCT query");
            if (c < 0 && o.e->kind == Expr::Var && o.e->slot < 0) throw CypherError("unknown variable " + o.e->name);
            ocol.push_back(c);
        }
        int64_t skip = eval_count(q.skip), limit = eval_count(q.limit);
        if (skip < 0) skip = 0;

        // Rows are (projected values, extra sort keys).
        std::vector<std::pair<std::vector<Value>, std::vector<Value>>> out;
        std::unordered_set<std::vector<Value>, VecHash, VecEq> distinct_seen;
        bool streaming = !aggregating && q.order.empty();
        Row row(slots_.size(), kNoNode);

        if (!aggregating) {
            auto sink = [&](const Row& r) {
                std::vector<Value> vals;
                for (auto& it : q.ret) vals.push_back(eval(it.e, r));
                if (q.distinct && !distinct_seen.insert(vals).second) return;
                std::vector<Value> keys;
                for (size_t i = 0; i < q.order.size(); ++i)
                    if (ocol[i] < 0) keys.push_back(eval(q.order[i].e, r));
                out.emplace_back(std::move(vals), std::move(keys));
                if (streaming && limit >= 0 && int64_t(out.size()) >= skip + limit) stop_ = true;
            };
            match(0, row, sink);
        } else {
            std::unordered_map<std::vector<Value>, size_t, VecHash, VecEq> groups;
            std::vector<std::vector<Value>> gkeys;
            std::vector<std::vector<Agg>> states;
            // One reused key buffer, and find() before inserting: emplace()
            // builds (allocates) a node even when the group already exists,
            // which made malloc/free the top of the profile.
            std::vector<Value> key;
            auto sink = [&](const Row& r) {
                key.clear();
                for (size_t c = 0; c < q.ret.size(); ++c)
                    if (!is_agg[c]) key.push_back(eval(q.ret[c].e, r));
                auto it = groups.find(key);
                if (it == groups.end()) {
                    it = groups.emplace(key, gkeys.size()).first;
                    gkeys.push_back(key);
                    states.emplace_back(q.ret.size());
                }
                auto& st = states[it->second];
                for (size_t c = 0; c < q.ret.size(); ++c) {
                    if (!is_agg[c]) continue;
                    const Expr& f = *q.ret[c].e;
                    Agg& a = st[c];
                    if (f.star) { ++a.count; continue; }
                    if (f.kids.size() != 1) throw CypherError(f.name + "() takes one argument");
                    Value v = eval(f.kids[0], r);
                    if (v.is_null()) continue;
                    if (f.distinct && !a.seen.insert(v).second) continue;
                    ++a.count;
                    if (f.name == "sum" || f.name == "avg") {
                        if (!v.is_num()) throw CypherError(f.name + "() of a non-number");
                        if (!v.is_int()) a.sum_float = true;
                        a.isum += v.is_int() ? v.as_int() : 0;
                        a.fsum += v.as_num();
                    } else if (f.name == "min" || f.name == "max") {
                        if (a.best.is_null()) a.best = v;
                        else {
                            bool ok;
                            int cmp = compare(v, a.best, ok);
                            if (!ok) cmp = order_less(v, a.best) ? -1 : 1;
                            if ((f.name == "min") ? cmp < 0 : cmp > 0) a.best = v;
                        }
                    }
                }
            };
            match(0, row, sink);
            // No grouping keys + no rows still yields one row (count = 0).
            bool has_keys = std::find(is_agg.begin(), is_agg.end(), false) != is_agg.end();
            if (gkeys.empty() && !has_keys) {
                gkeys.emplace_back();
                states.emplace_back(q.ret.size());
            }
            for (size_t gi = 0; gi < gkeys.size(); ++gi) {
                std::vector<Value> vals;
                size_t k = 0;
                for (size_t c = 0; c < q.ret.size(); ++c) {
                    if (!is_agg[c]) { vals.push_back(gkeys[gi][k++]); continue; }
                    const std::string& f = q.ret[c].e->name;
                    Agg& a = states[gi][c];
                    if (f == "count") vals.emplace_back(a.count);
                    else if (f == "sum") vals.push_back(a.sum_float ? Value(a.fsum) : Value(a.isum));
                    else if (f == "avg") vals.push_back(a.count ? Value(a.fsum / double(a.count)) : Value());
                    else vals.push_back(a.best);
                }
                if (q.distinct && !distinct_seen.insert(vals).second) continue;
                out.emplace_back(std::move(vals), std::vector<Value>{});
            }
        }

        if (!q.order.empty()) {
            auto less = [&](const auto& x, const auto& y) {
                size_t extra = 0;
                for (size_t i = 0; i < q.order.size(); ++i) {
                    const Value& a = ocol[i] >= 0 ? x.first[ocol[i]] : x.second[extra];
                    const Value& b = ocol[i] >= 0 ? y.first[ocol[i]] : y.second[extra];
                    if (ocol[i] < 0) ++extra;
                    if (order_less(a, b)) return !q.order[i].desc;
                    if (order_less(b, a)) return q.order[i].desc;
                }
                return false;
            };
            // ORDER BY ... LIMIT k only needs the first skip+k rows in order.
            size_t need = limit >= 0 ? size_t(skip + limit) : out.size();
            if (need < out.size()) std::partial_sort(out.begin(), out.begin() + long(need), out.end(), less);
            else std::stable_sort(out.begin(), out.end(), less);
        }
        for (int64_t i = skip; i < int64_t(out.size()); ++i) {
            if (limit >= 0 && i - skip >= limit) break;
            res_.rows.push_back(std::move(out[i].first));
        }
    }

    // MATCH (n...) WHERE ... RETURN ... ORDER BY vec_dist(n, $q) LIMIT k
    // becomes one filtered top-k vector search.
    bool try_vector_topk(const Query& q) {
        if (!vec_ || q.order.empty() || !q.limit || q.distinct) return false;
        const ExprPtr& o = q.order[0].e;
        if (o->kind != Expr::Func || o->name != "vec_dist" || q.order[0].desc || o->kids.size() != 2) return false;
        if (q.match.size() != 1 || q.match[0].nodes.size() != 1 || steps_.size() != 1) return false;
        if (o->kids[0]->kind != Expr::Var || o->kids[0]->slot != steps_[0].slot) return false;
        for (auto& r : q.ret)
            if (has_agg(r.e)) return false;
        const Step& s = steps_[0];
        int64_t k = eval_count(q.limit) + std::max<int64_t>(0, eval_count(q.skip));
        std::vector<float> qv = to_floats(eval(o->kids[1], {}));

        // Candidates the graph side can produce, filtered by labels/WHERE.
        // An all-nodes scan with no predicate is "no filter".
        bool unfiltered = s.src == Step::All && s.filters.empty() &&
                          (!s.check || (s.check->labels.empty() && s.check->props.empty()));
        std::vector<NodeId> matches;
        Row row(slots_.size(), kNoNode);
        if (!unfiltered) {
            auto sink = [&](const Row& r) { matches.push_back(r[s.slot]); };
            match(0, row, sink);
        }
        std::string strategy;
        auto hits = vec_->search(qv, size_t(k), unfiltered ? nullptr : &matches, strategy);
        res_.plan += "VectorTopK(k=" + std::to_string(k) + ", matches=" +
                     (unfiltered ? std::string("all") : std::to_string(matches.size())) + ", strategy=" + strategy + ")\n";
        for (auto& r : q.ret) res_.columns.push_back(r.alias);
        int64_t skip = std::max<int64_t>(0, eval_count(q.skip));
        for (size_t i = size_t(skip); i < hits.size(); ++i) {
            row[s.slot] = hits[i].second;
            std::vector<Value> vals;
            for (auto& it : q.ret) vals.push_back(eval(it.e, row));
            res_.rows.push_back(std::move(vals));
        }
        return true;
    }

    // ================= CREATE =================
    uint32_t commit(const std::string& r) {
        if (commit_) return commit_(r);
        return rec::apply(g_, r);
    }

    void run_create(const Query& q) {
        std::vector<Row> rows;
        Row row(slots_.size(), kNoNode);
        if (!q.match.empty()) {
            auto sink = [&](const Row& r) { rows.push_back(r); };
            match(0, row, sink);  // collect first: creating while iterating would invalidate adjacency
        } else {
            rows.push_back(row);
        }
        for (Row& r : rows) {
            for (auto& p : q.create) {
                for (auto& n : p.nodes) {
                    int s = slots_.at(n.var);
                    if (r[s] != kNoNode) {
                        if (!n.labels.empty() || !n.props.empty())
                            throw CypherError("cannot add labels/properties to bound variable " + n.var);
                        continue;
                    }
                    std::vector<std::pair<std::string, Value>> props;
                    for (auto& [k, e] : n.props) props.emplace_back(k, eval(e, r));
                    r[s] = commit(rec::create_node(n.labels, props));
                    ++res_.nodes_created;
                }
                for (size_t j = 0; j < p.rels.size(); ++j) {
                    const RelPat& rel = p.rels[j];
                    if (rel.dir == RelPat::Both) throw CypherError("CREATE needs a directed relationship");
                    if (rel.types.size() != 1 || rel.varlen) throw CypherError("CREATE needs exactly one relationship type");
                    NodeId a = r[slots_.at(p.nodes[j].var)], b = r[slots_.at(p.nodes[j + 1].var)];
                    if (rel.dir == RelPat::In) std::swap(a, b);
                    commit(rec::create_edge(a, b, rel.types[0]));
                    ++res_.edges_created;
                }
            }
        }
        if (q.has_return) {
            for (auto& it : q.ret) {
                if (has_agg(it.e)) throw CypherError("aggregates after CREATE are not supported");
                res_.columns.push_back(it.alias);
            }
            for (Row& r : rows) {
                std::vector<Value> vals;
                for (auto& it : q.ret) vals.push_back(eval(it.e, r));
                res_.rows.push_back(std::move(vals));
            }
        }
    }

    Graph& g_;
    const Params& params_;
    const Commit& commit_;
    const VectorHook* vec_;
    std::unordered_map<std::string, int> slots_;
    size_t match_vars_ = 0;
    std::unordered_map<std::string, NodeCheck> checks_;
    std::vector<Step> steps_;
    std::vector<EdgeId> used_;
    bool stop_ = false;
    Result res_;
};

} // namespace

Result run_cypher(Graph& g, std::string_view query, const Params& params, const Commit& commit,
                  const VectorHook* vec) {
    Query q = cy::parse(query);
    Exec ex(g, params, commit, vec);
    return ex.run(q);
}

} // namespace gv
