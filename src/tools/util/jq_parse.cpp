// jq_parse.cpp — lexer + recursive-descent parser for the jq subset.
//
// Precedence, lowest to highest (as in jq's grammar):
//   `|`  (right; `E as $x | body` binds over the rest)
//   `,`
//   `//` (right)
//   `=` `|=` `+=` `-=` `*=` `/=` `%=` `//=`  (nonassoc)
//   `or`, `and`
//   `==` `!=` `<` `<=` `>` `>=`  (nonassoc)
//   `+` `-`, then `*` `/` `%`
//   unary `-`, postfix (`.k`, `[e]`, `[a:b]`, `[]`, `?`), primary.

#include "jq_ast.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>

namespace mcp::tools::util::jq::detail {
namespace {

enum class T {
    End, Num, Str, Ident, Field, Var, Format, Dot, DotDot,
    LParen, RParen, LBrack, RBrack, LBrace, RBrace,
    Pipe, Comma, Colon, Semi, Question, Alt, Op, Assign,
};

struct StrPart { bool expr; std::string text; std::size_t col; };

struct Tok {
    T           t = T::End;
    std::string text;       // ident / field / var / format name / op
    double      num = 0;
    std::vector<StrPart> parts;   // Str
    std::size_t col = 0;
};

[[noreturn]] void syntax(std::size_t col, std::string msg) { throw SyntaxError{col, std::move(msg)}; }

void put_utf8(std::string& o, std::uint32_t cp) {
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Lexer {
public:
    Lexer(std::string_view s, std::size_t base) : s_(s), base_(base) {}

    std::vector<Tok> run() {
        std::vector<Tok> out;
        for (;;) {
            skip_ws();
            Tok t; t.col = base_ + i_ + 1;
            if (i_ >= s_.size()) { t.t = T::End; out.push_back(std::move(t)); return out; }
            const char c = s_[i_];
            if (c == '"') { t.t = T::Str; t.parts = string_lit(); out.push_back(std::move(t)); continue; }
            if (std::isdigit(static_cast<unsigned char>(c))) { t.t = T::Num; t.num = number(); out.push_back(std::move(t)); continue; }
            if (c == '.') {
                if (peek(1) == '.') { i_ += 2; t.t = T::DotDot; out.push_back(std::move(t)); continue; }
                if (ident_start(peek(1))) { ++i_; t.t = T::Field; t.text = ident(); out.push_back(std::move(t)); continue; }
                ++i_; t.t = T::Dot; out.push_back(std::move(t)); continue;
            }
            if (c == '$' && ident_start(peek(1))) { ++i_; t.t = T::Var; t.text = ident(); out.push_back(std::move(t)); continue; }
            if (c == '@' && ident_start(peek(1))) { ++i_; t.t = T::Format; t.text = ident(); out.push_back(std::move(t)); continue; }
            if (ident_start(c)) { t.t = T::Ident; t.text = ident(); out.push_back(std::move(t)); continue; }
            // multi-char operators first
            static constexpr std::string_view kOps3[] = {"//="};
            static constexpr std::string_view kOps2[] = {"|=", "+=", "-=", "*=", "/=", "%=", "==", "!=", "<=", ">=", "//"};
            bool matched = false;
            for (auto op : kOps3) if (s_.substr(i_, 3) == op) { t.t = T::Assign; t.text = op; i_ += 3; matched = true; break; }
            if (!matched) for (auto op : kOps2) if (s_.substr(i_, 2) == op) {
                t.text = op; i_ += 2; matched = true;
                t.t = op == "//" ? T::Alt : (op.back() == '=' && op != "==" && op != "!=" && op != "<=" && op != ">=") ? T::Assign : T::Op;
                break;
            }
            if (matched) { out.push_back(std::move(t)); continue; }
            ++i_;
            switch (c) {
                case '(': t.t = T::LParen; break;   case ')': t.t = T::RParen; break;
                case '[': t.t = T::LBrack; break;   case ']': t.t = T::RBrack; break;
                case '{': t.t = T::LBrace; break;   case '}': t.t = T::RBrace; break;
                case '|': t.t = T::Pipe; break;     case ',': t.t = T::Comma; break;
                case ':': t.t = T::Colon; break;    case ';': t.t = T::Semi; break;
                case '?': t.t = T::Question; break;
                case '=': t.t = T::Assign; t.text = "="; break;
                case '<': case '>': case '+': case '-': case '*': case '/': case '%':
                    t.t = T::Op; t.text = std::string(1, c); break;
                default: syntax(t.col, std::string("unexpected character '") + c + "'");
            }
            out.push_back(std::move(t));
        }
    }

private:
    char peek(std::size_t k) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }
    static bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
    static bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

    void skip_ws() {
        while (i_ < s_.size()) {
            if (std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
            else if (s_[i_] == '#') { while (i_ < s_.size() && s_[i_] != '\n') ++i_; }
            else break;
        }
    }
    std::string ident() {
        const std::size_t b = i_;
        while (i_ < s_.size() && ident_char(s_[i_])) ++i_;
        return std::string{s_.substr(b, i_ - b)};
    }
    double number() {
        const std::size_t b = i_;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        if (peek(0) == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            ++i_; while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if ((peek(0) == 'e' || peek(0) == 'E')
            && (std::isdigit(static_cast<unsigned char>(peek(1)))
                || ((peek(1) == '+' || peek(1) == '-') && std::isdigit(static_cast<unsigned char>(peek(2)))))) {
            i_ += 2; while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        return std::strtod(std::string{s_.substr(b, i_ - b)}.c_str(), nullptr);
    }
    std::uint32_t hex4(std::size_t col) {
        if (i_ + 4 > s_.size()) syntax(col, "truncated \\u escape");
        std::uint32_t v = 0;
        auto r = std::from_chars(s_.data() + i_, s_.data() + i_ + 4, v, 16);
        if (r.ptr != s_.data() + i_ + 4) syntax(col, "bad \\u escape");
        i_ += 4;
        return v;
    }
    // Skip a nested string literal (inside an interpolation) verbatim.
    void skip_string() {
        ++i_;   // opening quote
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '"') { ++i_; return; }
            if (c == '\\') {
                if (peek(1) == '(') { i_ += 2; skip_interp(); continue; }
                i_ += 2; continue;
            }
            ++i_;
        }
        syntax(base_ + i_, "unterminated string");
    }
    // Positioned just after `\(`; advance past the matching `)`.
    void skip_interp() {
        int depth = 1;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '"') { skip_string(); continue; }
            if (c == '(') ++depth;
            else if (c == ')' && --depth == 0) { ++i_; return; }
            ++i_;
        }
        syntax(base_ + i_, "unterminated \\( interpolation");
    }
    std::vector<StrPart> string_lit() {
        const std::size_t open = base_ + i_ + 1;
        ++i_;
        std::vector<StrPart> parts;
        std::string cur;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '"') {
                ++i_;
                if (!cur.empty() || parts.empty()) parts.push_back({false, std::move(cur), open});
                return parts;
            }
            if (c != '\\') { cur += c; ++i_; continue; }
            const std::size_t ecol = base_ + i_ + 1;
            const char e = peek(1);
            i_ += 2;
            switch (e) {
                case '"': cur += '"'; break;  case '\\': cur += '\\'; break; case '/': cur += '/'; break;
                case 'b': cur += '\b'; break; case 'f': cur += '\f'; break;  case 'n': cur += '\n'; break;
                case 'r': cur += '\r'; break; case 't': cur += '\t'; break;
                case 'u': {
                    std::uint32_t cp = hex4(ecol);
                    if (cp >= 0xD800 && cp <= 0xDBFF && peek(0) == '\\' && peek(1) == 'u') {
                        i_ += 2;
                        const std::uint32_t lo = hex4(ecol);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else { put_utf8(cur, 0xFFFD); cp = lo; }
                    }
                    put_utf8(cur, cp);
                    break;
                }
                case '(': {
                    if (!cur.empty()) parts.push_back({false, std::move(cur), open});
                    cur.clear();
                    const std::size_t b = i_;
                    skip_interp();
                    parts.push_back({true, std::string{s_.substr(b, i_ - 1 - b)}, base_ + b});
                    break;
                }
                default: syntax(ecol, std::string("invalid escape \\") + e);
            }
        }
        syntax(open, "unterminated string literal");
    }

    std::string_view s_;
    std::size_t      base_;
    std::size_t      i_ = 0;
};

bool is_keyword(std::string_view w) {
    return w == "then" || w == "elif" || w == "else" || w == "end" || w == "as" || w == "catch"
        || w == "and" || w == "or";
}

class Parser {
public:
    explicit Parser(std::vector<Tok> toks) : t_(std::move(toks)) {}

    NodeP program() {
        NodeP n = pipe();
        if (cur().t != T::End) syntax(cur().col, "unexpected " + show(cur()));
        return n;
    }

private:
    const Tok& cur() const { return t_[p_]; }
    const Tok& next() const { return t_[std::min(p_ + 1, t_.size() - 1)]; }
    bool is(T t) const { return cur().t == t; }
    bool is_ident(std::string_view w) const { return cur().t == T::Ident && cur().text == w; }
    bool is_op(std::string_view o) const { return cur().t == T::Op && cur().text == o; }
    const Tok& take() { return t_[p_ < t_.size() - 1 ? p_++ : p_]; }

    static std::string show(const Tok& t) {
        switch (t.t) {
            case T::End: return "end of query";
            case T::Ident: case T::Op: case T::Assign: return "'" + t.text + "'";
            case T::Field: return "'." + t.text + "'";
            case T::Var: return "'$" + t.text + "'";
            case T::Num: return "number";
            case T::Str: return "string";
            case T::RParen: return "')'";  case T::RBrack: return "']'"; case T::RBrace: return "'}'";
            case T::LParen: return "'('";  case T::LBrack: return "'['"; case T::LBrace: return "'{'";
            case T::Pipe: return "'|'";    case T::Comma: return "','";  case T::Colon: return "':'";
            case T::Semi: return "';'";    case T::Question: return "'?'"; case T::Alt: return "'//'";
            default: return "token";
        }
    }
    void expect(T t, std::string_view what) {
        if (!is(t)) syntax(cur().col, std::string("expected ") + std::string(what) + ", found " + show(cur()));
        take();
    }
    void expect_kw(std::string_view kw) {
        if (!is_ident(kw)) syntax(cur().col, "expected '" + std::string(kw) + "', found " + show(cur()));
        take();
    }

    static NodeP mk(K k, std::size_t col) { auto n = std::make_shared<Node>(); n->k = k; n->col = col; return n; }
    static NodeP mk2(K k, std::size_t col, NodeP a, NodeP b, std::string s = {}) {
        auto n = std::make_shared<Node>(); n->k = k; n->col = col; n->a = std::move(a); n->b = std::move(b);
        n->s = std::move(s); return n;
    }
    static NodeP lit(json v, std::size_t col) { auto n = std::make_shared<Node>(); n->k = K::Literal; n->lit = std::move(v); n->col = col; return n; }
    static NodeP identity(std::size_t col) { return mk(K::Identity, col); }

    NodeP pipe() {
        const std::size_t col = cur().col;
        if (is_ident("def")) syntax(col, "`def` (user functions) is not supported; inline the expression");
        NodeP lhs = comma();
        if (is_ident("as")) {
            take();
            if (is(T::LBrack) || is(T::LBrace))
                syntax(cur().col, "destructuring patterns in `as` are not supported; bind one $var");
            if (!is(T::Var)) syntax(cur().col, "expected $name after 'as'");
            std::string name = take().text;
            expect(T::Pipe, "'|' after `as $" + name + "`");
            auto n = std::make_shared<Node>();
            n->k = K::As; n->col = col; n->a = lhs; n->s = std::move(name); n->b = pipe();
            return n;
        }
        if (is(T::Pipe)) { take(); return mk2(K::Pipe, col, lhs, pipe()); }
        return lhs;
    }
    NodeP comma() {
        NodeP lhs = alt();
        while (is(T::Comma)) { const auto col = take().col; lhs = mk2(K::Comma, col, lhs, alt()); }
        return lhs;
    }
    NodeP alt() {
        NodeP lhs = assign();
        if (is(T::Alt)) { const auto col = take().col; return mk2(K::Alt, col, lhs, alt()); }
        return lhs;
    }
    NodeP assign() {
        NodeP lhs = or_();
        if (is(T::Assign)) {
            const Tok op = take();
            return mk2(K::Assign, op.col, lhs, alt(), op.text);
        }
        return lhs;
    }
    NodeP or_() {
        NodeP lhs = and_();
        while (is_ident("or")) { const auto col = take().col; lhs = mk2(K::Or, col, lhs, and_()); }
        return lhs;
    }
    NodeP and_() {
        NodeP lhs = cmp_();
        while (is_ident("and")) { const auto col = take().col; lhs = mk2(K::And, col, lhs, cmp_()); }
        return lhs;
    }
    NodeP cmp_() {
        NodeP lhs = add_();
        if (is(T::Op) && (cur().text == "==" || cur().text == "!=" || cur().text == "<"
                          || cur().text == "<=" || cur().text == ">" || cur().text == ">=")) {
            const Tok op = take();
            NodeP rhs = add_();
            if (is(T::Op) && (cur().text == "==" || cur().text == "!=" || cur().text == "<"
                              || cur().text == "<=" || cur().text == ">" || cur().text == ">="))
                syntax(cur().col, "comparisons don't chain; use `and`");
            return mk2(K::Bin, op.col, lhs, rhs, op.text);
        }
        return lhs;
    }
    NodeP add_() {
        NodeP lhs = mul_();
        while (is_op("+") || is_op("-")) { const Tok op = take(); lhs = mk2(K::Bin, op.col, lhs, mul_(), op.text); }
        return lhs;
    }
    NodeP mul_() {
        NodeP lhs = unary();
        while (is_op("*") || is_op("/") || is_op("%")) { const Tok op = take(); lhs = mk2(K::Bin, op.col, lhs, unary(), op.text); }
        return lhs;
    }
    NodeP unary() {
        if (is_op("-")) { const auto col = take().col; return mk2(K::Neg, col, postfix(), nullptr); }
        return postfix();
    }

    NodeP index_node(NodeP target, NodeP idx, std::size_t col) { return mk2(K::Index, col, std::move(target), std::move(idx)); }

    static NodeP slice_node(NodeP target, NodeP lo, NodeP hi, std::size_t col) {
        auto n = std::make_shared<Node>();
        n->k = K::Slice; n->col = col; n->a = std::move(target); n->b = std::move(lo); n->c = std::move(hi);
        return n;
    }

    NodeP bracket(NodeP target) {   // positioned just after '['
        const std::size_t col = cur().col;
        if (is(T::RBrack)) { take(); return mk2(K::Iterate, col, std::move(target), nullptr); }
        NodeP lo, hi;
        if (!is(T::Colon)) lo = pipe();
        if (is(T::Colon)) {
            take();
            if (!is(T::RBrack)) hi = pipe();
            expect(T::RBrack, "']'");
            return slice_node(std::move(target), std::move(lo), std::move(hi), col);
        }
        expect(T::RBrack, "']'");
        return index_node(std::move(target), std::move(lo), col);
    }

    NodeP postfix() {
        NodeP t = primary();
        for (;;) {
            if (is(T::Field)) { const Tok f = take(); t = index_node(t, lit(f.text, f.col), f.col); continue; }
            if (is(T::Dot) && next().t == T::Str) { take(); const auto col = cur().col; t = index_node(t, string_node(take(), ""), col); continue; }
            if (is(T::Dot) && next().t == T::LBrack) { take(); take(); t = bracket(t); continue; }
            if (is(T::LBrack)) { take(); t = bracket(t); continue; }
            if (is(T::Question)) { const auto col = take().col; t = mk2(K::Try, col, t, nullptr); continue; }
            return t;
        }
    }

    NodeP string_node(const Tok& tok, std::string fmt) {
        auto n = std::make_shared<Node>();
        n->k = K::Str; n->col = tok.col; n->s = std::move(fmt); n->lit = json::array();
        for (const auto& p : tok.parts) {
            if (p.expr) {
                Lexer lx(p.text, p.col);
                Parser sub(lx.run());
                n->args.push_back(sub.program());
                n->lit.push_back(false);
            } else {
                n->args.push_back(lit(p.text, p.col));
                n->lit.push_back(true);
            }
        }
        // A plain literal string folds to a Literal.
        if (n->s.empty() && n->args.size() == 1 && n->lit[0].get<bool>()) return n->args[0];
        return n;
    }

    NodeP object() {   // positioned just after '{'
        auto n = std::make_shared<Node>(); n->k = K::Object; n->col = cur().col;
        if (is(T::RBrace)) { take(); return n; }
        for (;;) {
            const std::size_t col = cur().col;
            NodeP key, val;
            if (is(T::Var)) {
                const std::string name = take().text;
                key = lit(name, col);
                auto v = std::make_shared<Node>(); v->k = K::Var; v->s = name; v->col = col; val = v;
            } else if (is(T::Ident)) {
                const std::string name = take().text;
                key = lit(name, col);
            } else if (is(T::Str)) {
                key = string_node(take(), "");
            } else if (is(T::Format) && next().t == T::Str) {
                std::string f = take().text; key = string_node(take(), f);
            } else if (is(T::Num)) {
                syntax(col, "object keys must be strings; write {\"" + std::to_string(static_cast<long long>(cur().num)) + "\": ...}");
            } else if (is(T::LParen)) {
                take(); key = pipe(); expect(T::RParen, "')'");
                if (!is(T::Colon)) syntax(cur().col, "a computed key `(...)` needs `: value`");
            } else {
                syntax(col, "expected an object key, found " + show(cur()));
            }
            if (is(T::Colon)) {
                take();
                val = objval();
            } else if (!val) {
                // shorthand {a} / {"a b"} → .a / .["a b"]
                val = index_node(identity(col), key, col);
            }
            n->args.push_back(std::move(key));
            n->args2.push_back(std::move(val));
            if (is(T::Comma)) { take(); continue; }
            expect(T::RBrace, "',' or '}'");
            return n;
        }
    }
    // jq restricts object values to `Term | Term`; accept any expression
    // without a top-level comma (the comma separates entries).
    NodeP objval() {
        NodeP v = alt();
        if (is(T::Pipe)) { const auto col = take().col; return mk2(K::Pipe, col, v, objval()); }
        return v;
    }

    NodeP primary() {
        const Tok& tk = cur();
        const std::size_t col = tk.col;
        switch (tk.t) {
            case T::Num: { const double d = take().num; return lit(num(d), col); }
            case T::Str: return string_node(take(), "");
            case T::Format: {
                std::string f = take().text;
                if (is(T::Str)) return string_node(take(), f);
                auto n = std::make_shared<Node>(); n->k = K::Format; n->s = std::move(f); n->col = col; return n;
            }
            case T::Dot:
                take();
                if (is(T::Str)) { const auto c2 = cur().col; return index_node(identity(col), string_node(take(), ""), c2); }
                return identity(col);   // `.[` handled by postfix
            case T::Field: { const Tok f = take(); return index_node(identity(col), lit(f.text, col), col); }
            case T::DotDot: take(); return mk(K::Recurse, col);
            case T::Var: {
                const std::string name = take().text;
                if (name == "__loc__") return lit(json{{"file", "<top-level>"}, {"line", 1}}, col);
                auto n = std::make_shared<Node>(); n->k = K::Var; n->s = name; n->col = col; return n;
            }
            case T::LParen: { take(); NodeP e = pipe(); expect(T::RParen, "')'"); return e; }
            case T::LBrack: {
                take();
                auto n = std::make_shared<Node>(); n->k = K::Array; n->col = col;
                if (!is(T::RBrack)) n->a = pipe();
                expect(T::RBrack, "']'");
                return n;
            }
            case T::LBrace: take(); return object();
            case T::Ident: return ident_term();
            default: syntax(col, "expected an expression, found " + show(tk));
        }
    }

    NodeP ident_term() {
        const Tok tk = take();
        const std::size_t col = tk.col;
        const std::string& w = tk.text;
        if (w == "true")  return lit(true, col);
        if (w == "false") return lit(false, col);
        if (w == "null")  return lit(nullptr, col);
        if (w == "if") {
            auto n = std::make_shared<Node>(); n->k = K::If; n->col = col;
            n->args.push_back(pipe()); expect_kw("then"); n->args2.push_back(pipe());
            while (is_ident("elif")) { take(); n->args.push_back(pipe()); expect_kw("then"); n->args2.push_back(pipe()); }
            if (is_ident("else")) { take(); n->a = pipe(); }
            expect_kw("end");
            return n;
        }
        if (w == "try") {
            NodeP body = postfix();
            NodeP handler;
            if (is_ident("catch")) { take(); handler = postfix(); }
            return mk2(K::Try, col, body, handler);
        }
        if (w == "reduce" || w == "foreach") {
            auto n = std::make_shared<Node>(); n->k = w == "reduce" ? K::Reduce : K::Foreach; n->col = col;
            n->a = postfix();
            expect_kw("as");
            if (!is(T::Var)) syntax(cur().col, "expected $name after 'as'");
            n->s = take().text;
            expect(T::LParen, "'('");
            n->b = pipe(); expect(T::Semi, "';'"); n->c = pipe();
            if (n->k == K::Foreach && is(T::Semi)) { take(); n->d = pipe(); }
            expect(T::RParen, "')'");
            return n;
        }
        if (w == "label" || w == "import" || w == "include")
            syntax(col, "`" + w + "` is not supported");
        if (is_keyword(w)) syntax(col, "unexpected keyword '" + w + "'");
        auto n = std::make_shared<Node>(); n->k = K::Call; n->s = w; n->col = col;
        if (is(T::LParen)) {
            take();
            n->args.push_back(pipe());
            while (is(T::Semi)) { take(); n->args.push_back(pipe()); }
            expect(T::RParen, "')' or ';'");
        }
        return n;
    }

    std::vector<Tok> t_;
    std::size_t      p_ = 0;
};

} // namespace

NodeP parse(std::string_view src, std::size_t col_base) {
    Lexer lx(src, col_base);
    Parser p(lx.run());
    return p.program();
}

} // namespace mcp::tools::util::jq::detail
