// Hand-written lexer and recursive-descent parser for the Cypher subset.
#include "cypher_ast.hpp"

#include "graphvec/cypher.hpp"

#include <cctype>
#include <cstdlib>

namespace gv::cy {
namespace {

struct Tok {
    enum Kind { Ident, Int, Float, Str, Param, Punct, End } kind;
    std::string text;
    size_t pos, end;
    bool quoted = false;  // `backtick` identifier: never a keyword
};

std::vector<Tok> lex(std::string_view s) {
    std::vector<Tok> out;
    size_t i = 0;
    while (true) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {  // line comment
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (i >= s.size()) break;
        size_t st = i;
        char c = s[i];
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
            out.push_back({Tok::Ident, std::string(s.substr(st, i - st)), st, i});
        } else if (c == '`') {
            size_t e = s.find('`', i + 1);
            if (e == std::string_view::npos) throw CypherError("unterminated `identifier`");
            out.push_back({Tok::Ident, std::string(s.substr(i + 1, e - i - 1)), st, e + 1, true});
            i = e + 1;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            bool flt = false;
            // "1..3" is a range, not the float "1." followed by ".3".
            if (i + 1 < s.size() && s[i] == '.' && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
                flt = true;
                ++i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            }
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                flt = true;
                ++i;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            }
            out.push_back({flt ? Tok::Float : Tok::Int, std::string(s.substr(st, i - st)), st, i});
        } else if (c == '\'' || c == '"') {
            std::string v;
            ++i;
            while (i < s.size() && s[i] != c) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    ++i;
                    v += s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i];
                } else {
                    v += s[i];
                }
                ++i;
            }
            if (i >= s.size()) throw CypherError("unterminated string");
            ++i;
            out.push_back({Tok::Str, v, st, i});
        } else if (c == '$') {
            ++i;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
            out.push_back({Tok::Param, std::string(s.substr(st + 1, i - st - 1)), st, i});
        } else {
            static const char* two[] = {"<>", "<=", ">=", "..", "!="};
            std::string t(1, c);
            for (const char* d : two)
                if (s.substr(i, 2) == d) t = d;
            i += t.size();
            out.push_back({Tok::Punct, t, st, i});
        }
    }
    out.push_back({Tok::End, "", s.size(), s.size()});
    return out;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

class Parser {
public:
    Parser(std::string_view src) : src_(src), t_(lex(src)) {}

    Query query() {
        Query q;
        if (kw("CREATE") && kw_at(1, "INDEX")) {
            next(); next();
            expect_kw("ON");
            expect(":");
            q.create_index = true;
            q.index_label = ident();
            expect("(");
            q.index_key = ident();
            expect(")");
            end();
            return q;
        }
        if (accept_kw("MATCH")) {
            q.match.push_back(pattern());
            while (accept(",")) q.match.push_back(pattern());
            if (accept_kw("WHERE")) q.where = expr();
        }
        if (accept_kw("CREATE")) {
            q.create.push_back(pattern());
            while (accept(",")) q.create.push_back(pattern());
        }
        if (accept_kw("RETURN")) {
            q.has_return = true;
            q.distinct = accept_kw("DISTINCT");
            do {
                ReturnItem it;
                it.e = expr();
                it.alias = accept_kw("AS") ? ident() : it.e->text;
                q.ret.push_back(it);
            } while (accept(","));
            if (accept_kw("ORDER")) {
                expect_kw("BY");
                do {
                    SortItem si;
                    si.e = expr();
                    if (accept_kw("DESC") || accept_kw("DESCENDING")) si.desc = true;
                    else if (!accept_kw("ASC")) accept_kw("ASCENDING");
                    q.order.push_back(si);
                } while (accept(","));
            }
            if (accept_kw("SKIP")) q.skip = expr();
            if (accept_kw("LIMIT")) q.limit = expr();
        }
        if (q.match.empty() && q.create.empty()) fail("expected MATCH or CREATE");
        end();
        return q;
    }

private:
    // ---- token helpers ----
    const Tok& cur() const { return t_[i_]; }
    void next() { if (cur().kind != Tok::End) ++i_; }
    bool is(const char* p) const { return cur().kind == Tok::Punct && cur().text == p; }
    bool is_at(size_t k, const char* p) const {
        return i_ + k < t_.size() && t_[i_ + k].kind == Tok::Punct && t_[i_ + k].text == p;
    }
    bool accept(const char* p) { if (is(p)) { next(); return true; } return false; }
    void expect(const char* p) { if (!accept(p)) fail(std::string("expected '") + p + "'"); }
    bool kw(const char* k) const { return cur().kind == Tok::Ident && !cur().quoted && ieq(cur().text, k); }
    bool kw_at(size_t k, const char* w) const {
        return i_ + k < t_.size() && t_[i_ + k].kind == Tok::Ident && ieq(t_[i_ + k].text, w);
    }
    bool accept_kw(const char* k) { if (kw(k)) { next(); return true; } return false; }
    void expect_kw(const char* k) { if (!accept_kw(k)) fail(std::string("expected ") + k); }
    std::string ident() {
        if (cur().kind != Tok::Ident) fail("expected identifier");
        std::string s = cur().text;
        next();
        return s;
    }
    void end() { if (cur().kind != Tok::End) fail("unexpected '" + cur().text + "'"); }
    [[noreturn]] void fail(const std::string& m) const {
        throw CypherError(m + " at offset " + std::to_string(cur().pos));
    }

    // ---- patterns ----
    Pattern pattern() {
        Pattern p;
        p.nodes.push_back(node());
        while (is("-") || is("<")) {
            p.rels.push_back(rel());
            p.nodes.push_back(node());
        }
        return p;
    }

    NodePat node() {
        NodePat n;
        expect("(");
        if (cur().kind == Tok::Ident) n.var = ident();
        while (accept(":")) n.labels.push_back(ident());
        if (is("{")) n.props = props();
        expect(")");
        return n;
    }

    std::vector<std::pair<std::string, ExprPtr>> props() {
        std::vector<std::pair<std::string, ExprPtr>> ps;
        expect("{");
        if (!is("}")) do {
                std::string k = ident();
                expect(":");
                ps.emplace_back(k, expr());
            } while (accept(","));
        expect("}");
        return ps;
    }

    // -[...]->   <-[...]-   -[...]-   -->   <--   --
    RelPat rel() {
        RelPat r;
        bool left = accept("<");
        expect("-");
        if (accept("[")) {
            if (cur().kind == Tok::Ident && !is_at(0, ":")) fail("relationship variables are not supported");
            if (accept(":")) {
                r.types.push_back(ident());
                while (accept("|")) { accept(":"); r.types.push_back(ident()); }
            }
            if (accept("*")) {
                r.varlen = true;
                r.min = 1;
                r.max = -1;
                if (cur().kind == Tok::Int) { r.min = r.max = std::atoi(cur().text.c_str()); next(); }
                if (accept("..")) {
                    r.max = -1;
                    if (cur().kind == Tok::Int) { r.max = std::atoi(cur().text.c_str()); next(); }
                }
                if (r.max < 0) fail("unbounded variable-length paths are not supported; give *min..max");
                if (r.min > r.max) fail("empty hop range");
            }
            if (is("{")) fail("relationship properties are not supported");
            expect("]");
        }
        expect("-");
        bool right = accept(">");
        if (left && right) fail("relationship cannot point both ways");
        r.dir = left ? RelPat::In : right ? RelPat::Out : RelPat::Both;
        return r;
    }

    // ---- expressions (lowest to highest precedence) ----
    ExprPtr mk(Expr::Kind k, std::string name, std::vector<ExprPtr> kids, size_t start) {
        auto e = std::make_shared<Expr>();
        e->kind = k;
        e->name = std::move(name);
        e->kids = std::move(kids);
        e->text = std::string(src_.substr(start, t_[i_ - 1].end - start));
        return e;
    }

    ExprPtr expr() { return or_expr(); }

    ExprPtr or_expr() {
        size_t s = cur().pos;
        ExprPtr l = xor_expr();
        while (accept_kw("OR")) l = mk(Expr::Or, "", {l, xor_expr()}, s);
        return l;
    }
    ExprPtr xor_expr() {
        size_t s = cur().pos;
        ExprPtr l = and_expr();
        while (accept_kw("XOR")) l = mk(Expr::Xor, "", {l, and_expr()}, s);
        return l;
    }
    ExprPtr and_expr() {
        size_t s = cur().pos;
        ExprPtr l = not_expr();
        while (accept_kw("AND")) l = mk(Expr::And, "", {l, not_expr()}, s);
        return l;
    }
    ExprPtr not_expr() {
        size_t s = cur().pos;
        if (accept_kw("NOT")) return mk(Expr::Not, "", {not_expr()}, s);
        return cmp_expr();
    }
    ExprPtr cmp_expr() {
        size_t s = cur().pos;
        ExprPtr l = add_expr();
        while (true) {
            static const char* ops[] = {"=", "<>", "!=", "<=", ">=", "<", ">"};
            bool hit = false;
            for (const char* op : ops)
                if (is(op)) {
                    next();
                    l = mk(Expr::Cmp, std::string(op) == "!=" ? "<>" : op, {l, add_expr()}, s);
                    hit = true;
                    break;
                }
            if (hit) continue;
            if (accept_kw("IN")) { l = mk(Expr::In, "", {l, add_expr()}, s); continue; }
            if (kw("IS")) {
                next();
                bool neg = accept_kw("NOT");
                expect_kw("NULL");
                l = mk(neg ? Expr::IsNotNull : Expr::IsNull, "", {l}, s);
                continue;
            }
            return l;
        }
    }
    ExprPtr add_expr() {
        size_t s = cur().pos;
        ExprPtr l = mul_expr();
        while (is("+") || is("-")) {
            std::string op = cur().text;
            next();
            l = mk(Expr::Arith, op, {l, mul_expr()}, s);
        }
        return l;
    }
    ExprPtr mul_expr() {
        size_t s = cur().pos;
        ExprPtr l = unary();
        while (is("*") || is("/") || is("%")) {
            std::string op = cur().text;
            next();
            l = mk(Expr::Arith, op, {l, unary()}, s);
        }
        return l;
    }
    ExprPtr unary() {
        size_t s = cur().pos;
        if (accept("-")) return mk(Expr::Neg, "", {unary()}, s);
        return postfix();
    }
    ExprPtr postfix() {
        size_t s = cur().pos;
        ExprPtr e = primary();
        while (accept(".")) e = mk(Expr::Prop, ident(), {e}, s);
        return e;
    }
    ExprPtr primary() {
        size_t s = cur().pos;
        const Tok& t = cur();
        if (t.kind == Tok::Int) {
            next();
            auto e = mk(Expr::Lit, "", {}, s);
            e->lit = Value(int64_t(std::strtoll(t.text.c_str(), nullptr, 10)));
            return e;
        }
        if (t.kind == Tok::Float) {
            next();
            auto e = mk(Expr::Lit, "", {}, s);
            e->lit = Value(std::strtod(t.text.c_str(), nullptr));
            return e;
        }
        if (t.kind == Tok::Str) {
            std::string v = t.text;
            next();
            auto e = mk(Expr::Lit, "", {}, s);
            e->lit = Value(v);
            return e;
        }
        if (t.kind == Tok::Param) {
            std::string n = t.text;
            next();
            return mk(Expr::Param, n, {}, s);
        }
        if (accept("(")) {
            ExprPtr e = expr();
            expect(")");
            return e;
        }
        if (accept("[")) {
            std::vector<ExprPtr> items;
            if (!is("]")) do items.push_back(expr()); while (accept(","));
            expect("]");
            return mk(Expr::ListLit, "", items, s);
        }
        if (t.kind == Tok::Ident) {
            if (!t.quoted) {
                if (kw("TRUE") || kw("FALSE")) {
                    bool b = kw("TRUE");
                    next();
                    auto e = mk(Expr::Lit, "", {}, s);
                    e->lit = Value(b);
                    return e;
                }
                if (kw("NULL")) { next(); return mk(Expr::Lit, "", {}, s); }
            }
            std::string name = ident();
            if (accept("(")) {
                std::string lname;
                for (char c : name) lname += char(std::tolower(static_cast<unsigned char>(c)));
                bool distinct = accept_kw("DISTINCT");
                bool star = false;
                std::vector<ExprPtr> args;
                if (accept("*")) star = true;
                else if (!is(")")) do args.push_back(expr()); while (accept(","));
                expect(")");
                auto e = mk(Expr::Func, lname, args, s);
                e->distinct = distinct;
                e->star = star;
                return e;
            }
            return mk(Expr::Var, name, {}, s);
        }
        fail("unexpected '" + t.text + "'");
    }

    std::string_view src_;
    std::vector<Tok> t_;
    size_t i_ = 0;
};

} // namespace

Query parse(std::string_view src) { return Parser(src).query(); }

} // namespace gv::cy
