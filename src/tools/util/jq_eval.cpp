// jq_eval.cpp — value helpers, the generator evaluator, and path expressions.
//
// Every filter is evaluated in "callback" style: eval(node, input) calls `out`
// once per output, so generators (`.[]`, `range`, `,`) stream without
// materialising. Error handling that must ignore a CONSUMER's errors (try,
// `?`, `//`) wraps its output with downstream_wrap; see jq_ast.hpp.

#include "jq_ast.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mcp::tools::util::jq::detail {

// ── value helpers ──────────────────────────────────────────────────────────
void fail(std::string msg) { throw JqError{json(std::move(msg))}; }

bool truthy(const json& v) { return !(v.is_null() || (v.is_boolean() && !v.get<bool>())); }

static int rank(const json& v) {
    if (v.is_null()) return 0;
    if (v.is_boolean()) return v.get<bool>() ? 2 : 1;
    if (v.is_number()) return 3;
    if (v.is_string()) return 4;
    if (v.is_array()) return 5;
    return 6;
}

double to_double(const json& v) {
    if (v.is_number_unsigned()) return static_cast<double>(v.get<std::uint64_t>());
    if (v.is_number_integer()) return static_cast<double>(v.get<std::int64_t>());
    return v.get<double>();
}

static std::vector<std::string> sorted_keys(const json& o) {
    std::vector<std::string> k;
    k.reserve(o.size());
    for (auto it = o.begin(); it != o.end(); ++it) k.push_back(it.key());
    std::sort(k.begin(), k.end());
    return k;
}

int cmp(const json& a, const json& b) {
    const int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    switch (ra) {
        case 3: {
            const double x = to_double(a), y = to_double(b);
            return x < y ? -1 : x > y ? 1 : 0;
        }
        case 4: {
            const int c = a.get_ref<const std::string&>().compare(b.get_ref<const std::string&>());
            return c < 0 ? -1 : c > 0 ? 1 : 0;
        }
        case 5: {
            const std::size_t n = std::min(a.size(), b.size());
            for (std::size_t i = 0; i < n; ++i) if (int c = cmp(a[i], b[i])) return c;
            return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
        }
        case 6: {
            const auto ka = sorted_keys(a), kb = sorted_keys(b);
            json ja = ka, jb = kb;
            if (int c = cmp(ja, jb)) return c;
            for (const auto& k : ka) if (int c = cmp(a.at(k), b.at(k))) return c;
            return 0;
        }
        default: return 0;
    }
}

bool equal(const json& a, const json& b) { return cmp(a, b) == 0; }

json num(double d) {
    if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9007199254740992.0)
        return json(static_cast<std::int64_t>(d));
    if (std::isnan(d)) return json(nullptr);     // jq prints nan as null
    if (std::isinf(d)) return json(d > 0 ? std::numeric_limits<double>::max()
                                         : -std::numeric_limits<double>::max());
    return json(d);
}

std::string type_of(const json& v) {
    switch (rank(v)) {
        case 0: return "null"; case 1: case 2: return "boolean"; case 3: return "number";
        case 4: return "string"; case 5: return "array"; default: return "object";
    }
}

std::string describe(const json& v) {
    std::string d = v.dump(-1, ' ', false, json::error_handler_t::replace);
    if (d.size() > 11) d = d.substr(0, 10) + "...";
    return type_of(v) + " (" + d + ")";
}

std::string tostring(const json& v) {
    return v.is_string() ? v.get<std::string>()
                         : v.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::size_t cp_len(std::string_view s) {
    std::size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}
std::size_t cp_to_byte(std::string_view s, std::size_t cp) {
    std::size_t seen = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (seen == cp) return i;
            ++seen;
        }
    }
    return s.size();
}
std::size_t byte_to_cp(std::string_view s, std::size_t byte) { return cp_len(s.substr(0, std::min(byte, s.size()))); }

static long long clamp_index(double d, std::size_t n, bool ceil_it = false) {
    long long i = static_cast<long long>(ceil_it ? std::ceil(d) : std::floor(d));
    if (i < 0) i += static_cast<long long>(n);
    return std::clamp<long long>(i, 0, static_cast<long long>(n));
}

json index_value(const json& t, const json& idx) {
    if (t.is_null()) {
        if (idx.is_string() || idx.is_number() || idx.is_null()) return nullptr;
        if (idx.is_object()) return nullptr;
        fail("Cannot index null with " + type_of(idx));
    }
    if (t.is_object()) {
        if (!idx.is_string()) fail("Cannot index object with " + type_of(idx));
        auto it = t.find(idx.get<std::string>());
        return it == t.end() ? json(nullptr) : *it;
    }
    if (t.is_array()) {
        if (idx.is_number()) {
            long long i = static_cast<long long>(std::floor(to_double(idx)));
            if (i < 0) i += static_cast<long long>(t.size());
            if (i < 0 || i >= static_cast<long long>(t.size())) return nullptr;
            return t[static_cast<std::size_t>(i)];
        }
        if (idx.is_array()) {   // .[[x,y]] — indices of the sub-array
            json out = json::array();
            if (idx.empty()) return nullptr;
            for (std::size_t i = 0; i + idx.size() <= t.size(); ++i) {
                bool m = true;
                for (std::size_t j = 0; j < idx.size() && m; ++j) m = equal(t[i + j], idx[j]);
                if (m) out.push_back(i);
            }
            return out;
        }
        if (idx.is_object() && (idx.contains("start") || idx.contains("end")))
            return slice_value(t, idx.value("start", json(nullptr)), idx.value("end", json(nullptr)));
        fail("Cannot index array with " + (idx.is_string() ? "string \"" + idx.get<std::string>() + "\"" : type_of(idx)));
    }
    if (idx.is_string()) fail("Cannot index " + type_of(t) + " with \"" + idx.get<std::string>() + "\"");
    fail("Cannot index " + type_of(t) + " with " + type_of(idx));
}

json slice_value(const json& t, const json& lo, const json& hi) {
    if (t.is_null()) return nullptr;
    if ((!lo.is_null() && !lo.is_number()) || (!hi.is_null() && !hi.is_number()))
        fail("Start and end indices of an array slice must be numbers");
    if (t.is_array()) {
        const std::size_t n = t.size();
        const auto a = lo.is_null() ? 0 : clamp_index(to_double(lo), n);
        const auto b = hi.is_null() ? static_cast<long long>(n) : clamp_index(to_double(hi), n, true);
        json out = json::array();
        for (long long i = a; i < b; ++i) out.push_back(t[static_cast<std::size_t>(i)]);
        return out;
    }
    if (t.is_string()) {
        const auto& s = t.get_ref<const std::string&>();
        const std::size_t n = cp_len(s);
        const auto a = lo.is_null() ? 0 : clamp_index(to_double(lo), n);
        const auto b = hi.is_null() ? static_cast<long long>(n) : clamp_index(to_double(hi), n, true);
        if (b <= a) return "";
        const auto ba = cp_to_byte(s, static_cast<std::size_t>(a));
        const auto bb = cp_to_byte(s, static_cast<std::size_t>(b));
        return s.substr(ba, bb - ba);
    }
    fail("Cannot index " + type_of(t) + " with object");
}

static json deep_merge(json a, const json& b) {
    for (auto it = b.begin(); it != b.end(); ++it) {
        auto ai = a.find(it.key());
        if (ai != a.end() && ai->is_object() && it->is_object()) *ai = deep_merge(*ai, *it);
        else a[it.key()] = *it;
    }
    return a;
}

json binop(const std::string& op, const json& l, const json& r) {
    if (op == "==") return equal(l, r);
    if (op == "!=") return !equal(l, r);
    if (op == "<")  return cmp(l, r) < 0;
    if (op == "<=") return cmp(l, r) <= 0;
    if (op == ">")  return cmp(l, r) > 0;
    if (op == ">=") return cmp(l, r) >= 0;
    const auto both = [&](auto pred) { return pred(l) && pred(r); };
    if (op == "+") {
        if (l.is_null()) return r;
        if (r.is_null()) return l;
        if (both([](const json& v) { return v.is_number(); })) return num(to_double(l) + to_double(r));
        if (both([](const json& v) { return v.is_string(); })) {
            if (l.get_ref<const std::string&>().size() + r.get_ref<const std::string&>().size() > (64u << 20))
                throw Fatal{"string too long"};
            return l.get<std::string>() + r.get<std::string>();
        }
        if (both([](const json& v) { return v.is_array(); })) {
            json o = l; for (const auto& e : r) o.push_back(e); return o;
        }
        if (both([](const json& v) { return v.is_object(); })) {
            json o = l; for (auto it = r.begin(); it != r.end(); ++it) o[it.key()] = *it; return o;
        }
        fail(describe(l) + " and " + describe(r) + " cannot be added");
    }
    if (op == "-") {
        if (both([](const json& v) { return v.is_number(); })) return num(to_double(l) - to_double(r));
        if (both([](const json& v) { return v.is_array(); })) {
            json o = json::array();
            for (const auto& e : l) {
                bool drop = false;
                for (const auto& x : r) if (equal(e, x)) { drop = true; break; }
                if (!drop) o.push_back(e);
            }
            return o;
        }
        fail(describe(l) + " and " + describe(r) + " cannot be subtracted");
    }
    if (op == "*") {
        if (both([](const json& v) { return v.is_number(); })) return num(to_double(l) * to_double(r));
        if ((l.is_string() && r.is_number()) || (l.is_number() && r.is_string())) {
            const std::string& s = l.is_string() ? l.get_ref<const std::string&>() : r.get_ref<const std::string&>();
            const double n = to_double(l.is_number() ? l : r);
            if (n <= 0) return nullptr;
            const auto times = static_cast<std::size_t>(std::ceil(n));
            if (s.size() * times > (64u << 20)) throw Fatal{"string too long"};
            std::string o; o.reserve(s.size() * times);
            for (std::size_t i = 0; i < times; ++i) o += s;
            return o;
        }
        if (both([](const json& v) { return v.is_object(); })) return deep_merge(l, r);
        fail(describe(l) + " and " + describe(r) + " cannot be multiplied");
    }
    if (op == "/") {
        if (both([](const json& v) { return v.is_number(); })) {
            if (to_double(r) == 0) fail(describe(l) + " and " + describe(r) + " cannot be divided because the divisor is zero");
            return num(to_double(l) / to_double(r));
        }
        if (both([](const json& v) { return v.is_string(); })) {
            const auto& s = l.get_ref<const std::string&>();
            const auto& sep = r.get_ref<const std::string&>();
            json o = json::array();
            if (s.empty()) return o;
            if (sep.empty()) {   // split into characters
                for (std::size_t i = 0; i < s.size();) {
                    std::size_t j = i + 1;
                    while (j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) ++j;
                    o.push_back(s.substr(i, j - i)); i = j;
                }
                return o;
            }
            std::size_t p = 0;
            for (;;) {
                const auto q = s.find(sep, p);
                if (q == std::string::npos) { o.push_back(s.substr(p)); break; }
                o.push_back(s.substr(p, q - p)); p = q + sep.size();
            }
            return o;
        }
        fail(describe(l) + " and " + describe(r) + " cannot be divided");
    }
    if (op == "%") {
        if (both([](const json& v) { return v.is_number(); })) {
            const auto a = static_cast<long long>(to_double(l));
            const auto b = static_cast<long long>(to_double(r));
            if (b == 0) fail(describe(l) + " and " + describe(r) + " cannot be divided because the divisor is zero");
            return json(a % (b < 0 ? -b : b));
        }
        fail(describe(l) + " and " + describe(r) + " cannot be divided");
    }
    fail("unknown operator " + op);
}

// ── paths ──────────────────────────────────────────────────────────────────
json getpath(const json& v, const json& path) {
    const json* cur = &v;
    json tmp;
    for (const auto& k : path) {
        if (cur->is_null()) return nullptr;
        tmp = index_value(*cur, k);
        cur = &tmp;
    }
    return *cur;
}

void setpath(json& v, const json& path, std::size_t i, const json& val) {
    if (i == path.size()) { v = val; return; }
    const json& k = path[i];
    if (k.is_string()) {
        if (v.is_null()) v = json::object();
        if (!v.is_object()) fail("Cannot index " + type_of(v) + " with \"" + k.get<std::string>() + "\"");
        setpath(v[k.get<std::string>()], path, i + 1, val);
    } else if (k.is_number()) {
        if (v.is_null()) v = json::array();
        if (!v.is_array()) fail("Cannot index " + type_of(v) + " with number");
        long long idx = static_cast<long long>(std::floor(to_double(k)));
        if (idx < 0) { idx += static_cast<long long>(v.size()); if (idx < 0) fail("Out of bounds negative array index"); }
        if (idx > 1'000'000) fail("Array index too large");
        while (static_cast<long long>(v.size()) <= idx) v.push_back(nullptr);
        setpath(v[static_cast<std::size_t>(idx)], path, i + 1, val);
    } else if (k.is_object()) {
        if (v.is_null()) v = json::array();
        if (!v.is_array()) fail("Cannot update field at object index of " + type_of(v));
        const std::size_t n = v.size();
        const auto a = k.value("start", json(nullptr)).is_null() ? 0 : clamp_index(to_double(k["start"]), n);
        auto b = k.value("end", json(nullptr)).is_null() ? static_cast<long long>(n) : clamp_index(to_double(k["end"]), n, true);
        if (b < a) b = a;
        json cur = slice_value(v, k.value("start", json(nullptr)), k.value("end", json(nullptr)));
        setpath(cur, path, i + 1, val);
        if (!cur.is_array()) fail("A slice of an array can only be assigned another array");
        json out = json::array();
        for (long long j = 0; j < a; ++j) out.push_back(v[static_cast<std::size_t>(j)]);
        for (const auto& e : cur) out.push_back(e);
        for (long long j = b; j < static_cast<long long>(n); ++j) out.push_back(v[static_cast<std::size_t>(j)]);
        v = std::move(out);
    } else {
        fail("Invalid path component " + describe(k));
    }
}

static void delpath(json& v, const json& path, std::size_t i) {
    if (v.is_null()) return;
    const json& k = path[i];
    const bool last = i + 1 == path.size();
    if (k.is_string()) {
        if (!v.is_object()) fail("Cannot delete field at object index of " + type_of(v));
        auto it = v.find(k.get<std::string>());
        if (it == v.end()) return;
        if (last) v.erase(it); else delpath(*it, path, i + 1);
    } else if (k.is_number()) {
        if (!v.is_array()) fail("Cannot delete field at index of " + type_of(v));
        long long idx = static_cast<long long>(std::floor(to_double(k)));
        if (idx < 0) idx += static_cast<long long>(v.size());
        if (idx < 0 || idx >= static_cast<long long>(v.size())) return;
        if (last) v.erase(static_cast<std::size_t>(idx)); else delpath(v[static_cast<std::size_t>(idx)], path, i + 1);
    } else if (k.is_object()) {
        if (!v.is_array()) fail("Cannot delete slice of " + type_of(v));
        const std::size_t n = v.size();
        const auto a = k.value("start", json(nullptr)).is_null() ? 0 : clamp_index(to_double(k["start"]), n);
        const auto b = k.value("end", json(nullptr)).is_null() ? static_cast<long long>(n) : clamp_index(to_double(k["end"]), n, true);
        if (!last) fail("Cannot delete a path through a slice");
        json out = json::array();
        for (long long j = 0; j < static_cast<long long>(n); ++j) if (j < a || j >= b) out.push_back(v[static_cast<std::size_t>(j)]);
        v = std::move(out);
    } else {
        fail("Invalid path component " + describe(k));
    }
}

json delpaths(json v, json paths) {
    if (!paths.is_array()) fail("Paths must be specified as an array");
    std::vector<json> ps(paths.begin(), paths.end());
    // Longest/last first, so deleting one never shifts another.
    std::sort(ps.begin(), ps.end(), [](const json& a, const json& b) { return cmp(a, b) > 0; });
    for (const auto& p : ps) {
        if (!p.is_array()) fail("Path must be specified as an array");
        if (p.empty()) { v = nullptr; continue; }
        delpath(v, p, 0);
    }
    return v;
}

// ── evaluator ──────────────────────────────────────────────────────────────
namespace {

EnvP bind(const EnvP& env, std::string name, json v) {
    auto e = std::make_shared<Env>();
    e->name = std::move(name); e->value = std::move(v); e->parent = env;
    return e;
}

json lookup(const EnvP& env, const std::string& name) {
    for (const Env* e = env.get(); e; e = e->parent.get()) if (e->name == name) return e->value;
    if (name == "ENV") return json::object();   // never leak the host environment
    fail("$" + name + " is not defined");
}

void recurse_all(Ctx& cx, const json& v, const Out& out) {
    DepthGuard g(cx);
    out(v);
    if (v.is_array()) for (const auto& e : v) recurse_all(cx, e, out);
    else if (v.is_object()) for (const auto& e : v) recurse_all(cx, e, out);
}

void recurse_paths(Ctx& cx, const json& v, const json& base, const PathOut& out) {
    DepthGuard g(cx);
    out(base, v);
    if (v.is_array()) {
        for (std::size_t i = 0; i < v.size(); ++i) { json p = base; p.push_back(i); recurse_paths(cx, v[i], p, out); }
    } else if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it) { json p = base; p.push_back(it.key()); recurse_paths(cx, *it, p, out); }
    }
}

void eval_iterate(const json& t, const Out& out) {
    if (t.is_array())       for (const auto& e : t) out(e);
    else if (t.is_object()) for (const auto& e : t) out(e);
    else fail("Cannot iterate over " + describe(t));
}

void eval_str(Ctx& cx, const Node& n, const json& in, const EnvP& env, const Out& out,
              std::size_t i, std::string acc) {
    if (i == n.args.size()) { out(json(std::move(acc))); return; }
    if (n.lit[i].get<bool>()) {
        acc += n.args[i]->lit.get_ref<const std::string&>();
        eval_str(cx, n, in, env, out, i + 1, std::move(acc));
        return;
    }
    eval(cx, *n.args[i], in, env, [&](const json& v) {
        std::string piece = n.s.empty() ? tostring(v) : apply_format(n.s, v);
        eval_str(cx, n, in, env, out, i + 1, acc + piece);
    });
}

void eval_object(Ctx& cx, const Node& n, const json& in, const EnvP& env, const Out& out,
                 std::size_t i, const json& acc) {
    if (i == n.args.size()) { out(acc); return; }
    eval(cx, *n.args[i], in, env, [&](const json& k) {
        if (!k.is_string()) fail("Object keys must be strings, got " + describe(k));
        eval(cx, *n.args2[i], in, env, [&](const json& v) {
            json next = acc;
            next[k.get<std::string>()] = v;
            eval_object(cx, n, in, env, out, i + 1, next);
        });
    });
}

void eval_if(Ctx& cx, const Node& n, const json& in, const EnvP& env, const Out& out, std::size_t i) {
    if (i == n.args.size()) {
        if (n.a) eval(cx, *n.a, in, env, out); else out(in);
        return;
    }
    eval(cx, *n.args[i], in, env, [&](const json& c) {
        if (truthy(c)) eval(cx, *n.args2[i], in, env, out);
        else eval_if(cx, n, in, env, out, i + 1);
    });
}

void eval_try(Ctx& cx, const Node& n, const json& in, const EnvP& env, const Out& out) {
    const auto id = cx.next_id++;
    const Out wrapped = downstream_wrap(id, out);
    try {
        eval(cx, *n.a, in, env, wrapped);
    } catch (Downstream& d) {
        if (d.id != id) throw;
        throw JqError{std::move(d.value)};
    } catch (JqError& e) {
        if (n.b) eval(cx, *n.b, e.value, env, out);
    }
}

void eval_alt(Ctx& cx, const Node& n, const json& in, const EnvP& env, const Out& out) {
    // jq 1.8: `a // b` yields a's truthy outputs, else b's. Errors raised
    // by `a` propagate (use `a? // b` to suppress them), and so do errors
    // raised downstream of an output already emitted.
    bool any = false;
    eval(cx, *n.a, in, env, [&](const json& v) { if (truthy(v)) { any = true; out(v); } });
    if (!any) eval(cx, *n.b, in, env, out);
}

void eval_assign(Ctx& cx, const Node& n, const json& in, const EnvP& env, const Out& out) {
    const std::string& op = n.s;
    auto collect_paths = [&](const json& root) {
        std::vector<json> ps;
        eval_paths(cx, *n.a, root, json::array(), env, [&](const json& p, const json&) { ps.push_back(p); });
        return ps;
    };
    if (op == "|=") {
        json result = in;
        json dels = json::array();
        for (const auto& p : collect_paths(in)) {
            const json old = getpath(result, p);
            const auto id = cx.next_id++;
            bool got = false; json nv;
            try {
                eval(cx, *n.b, old, env, [&](const json& v) { nv = v; got = true; throw Stop{id}; });
            } catch (Stop& s) { if (s.id != id) throw; }
            if (got) setpath(result, p, 0, nv); else dels.push_back(p);
        }
        out(dels.empty() ? result : delpaths(result, dels));
        return;
    }
    eval(cx, *n.b, in, env, [&](const json& rhs) {
        json result = in;
        for (const auto& p : collect_paths(in)) {
            if (op == "=") { setpath(result, p, 0, rhs); continue; }
            const json old = getpath(result, p);
            json nv;
            if (op == "//=") nv = truthy(old) ? old : rhs;
            else nv = binop(op.substr(0, op.size() - 1), old, rhs);
            setpath(result, p, 0, nv);
        }
        out(result);
    });
}

void eval_reduce(Ctx& cx, const Node& n, const json& in, const EnvP& env, const Out& out) {
    eval(cx, *n.b, in, env, [&](const json& init) {
        json acc = init;
        std::vector<json> emitted;
        eval(cx, *n.a, in, env, [&](const json& x) {
            const EnvP e2 = bind(env, n.s, x);
            if (n.k == K::Reduce) {
                bool got = false; json last;
                eval(cx, *n.c, acc, e2, [&](const json& u) { last = u; got = true; });
                acc = got ? last : json(nullptr);
            } else {
                eval(cx, *n.c, acc, e2, [&](const json& u) {
                    acc = u;
                    if (n.d) eval(cx, *n.d, acc, e2, out); else out(acc);
                });
            }
        });
        if (n.k == K::Reduce) out(acc);
    });
}

} // namespace

void eval(Ctx& cx, const Node& n, const json& in, const EnvP& env, const Out& out) {
    DepthGuard g(cx);
    switch (n.k) {
        case K::Identity: out(in); return;
        case K::Recurse:  recurse_all(cx, in, out); return;
        case K::Literal:  out(n.lit); return;
        case K::Index:
            eval(cx, *n.a, in, env, [&](const json& t) {
                eval(cx, *n.b, in, env, [&](const json& idx) { out(index_value(t, idx)); });
            });
            return;
        case K::Slice:
            eval(cx, *n.a, in, env, [&](const json& t) {
                auto with_hi = [&](const json& lo) {
                    if (!n.c) { out(slice_value(t, lo, nullptr)); return; }
                    eval(cx, *n.c, in, env, [&](const json& hi) { out(slice_value(t, lo, hi)); });
                };
                if (!n.b) with_hi(nullptr); else eval(cx, *n.b, in, env, with_hi);
            });
            return;
        case K::Iterate:
            eval(cx, *n.a, in, env, [&](const json& t) { eval_iterate(t, out); });
            return;
        case K::Str:    eval_str(cx, n, in, env, out, 0, {}); return;
        case K::Format: out(json(apply_format(n.s, in))); return;
        case K::Pipe:
            eval(cx, *n.a, in, env, [&](const json& v) { eval(cx, *n.b, v, env, out); });
            return;
        case K::Comma:
            eval(cx, *n.a, in, env, out);
            eval(cx, *n.b, in, env, out);
            return;
        case K::Alt: eval_alt(cx, n, in, env, out); return;
        case K::And:
        case K::Or:
            eval(cx, *n.a, in, env, [&](const json& l) {
                const bool lt = truthy(l);
                if (n.k == K::And && !lt) { out(false); return; }
                if (n.k == K::Or && lt)   { out(true);  return; }
                eval(cx, *n.b, in, env, [&](const json& r) { out(truthy(r)); });
            });
            return;
        case K::Neg:
            eval(cx, *n.a, in, env, [&](const json& v) {
                if (!v.is_number()) fail(describe(v) + " cannot be negated");
                out(num(-to_double(v)));
            });
            return;
        case K::Bin:
            eval(cx, *n.b, in, env, [&](const json& r) {
                eval(cx, *n.a, in, env, [&](const json& l) { out(binop(n.s, l, r)); });
            });
            return;
        case K::Array: {
            json arr = json::array();
            if (n.a) eval(cx, *n.a, in, env, [&](const json& v) {
                tick(cx);
                if (arr.size() >= kMaxCollection)
                    throw Fatal{"array would exceed " + std::to_string(kMaxCollection) + " elements"};
                arr.push_back(v);
            });
            out(arr);
            return;
        }
        case K::Object: eval_object(cx, n, in, env, out, 0, json::object()); return;
        case K::If:     eval_if(cx, n, in, env, out, 0); return;
        case K::Try:    eval_try(cx, n, in, env, out); return;
        case K::Reduce:
        case K::Foreach: eval_reduce(cx, n, in, env, out); return;
        case K::As:
            eval(cx, *n.a, in, env, [&](const json& v) { eval(cx, *n.b, in, bind(env, n.s, v), out); });
            return;
        case K::Var:    out(lookup(env, n.s)); return;
        case K::Assign: eval_assign(cx, n, in, env, out); return;
        case K::Call:
            if (!call_builtin(cx, n, in, env, out))
                fail(n.s + "/" + std::to_string(n.args.size()) + " is not a known function");
            return;
    }
}

void eval_paths(Ctx& cx, const Node& n, const json& in, const json& base, const EnvP& env,
                const PathOut& out) {
    DepthGuard g(cx);
    switch (n.k) {
        case K::Identity: out(base, in); return;
        case K::Recurse:  recurse_paths(cx, in, base, out); return;
        case K::Index:
            eval_paths(cx, *n.a, in, base, env, [&](const json& p, const json& v) {
                eval(cx, *n.b, in, env, [&](const json& idx) {
                    json key = idx;
                    if (idx.is_number() && v.is_array()) {
                        long long i = static_cast<long long>(std::floor(to_double(idx)));
                        if (i < 0) i += static_cast<long long>(v.size());
                        key = i;
                    }
                    json np = p; np.push_back(key);
                    out(np, index_value(v, idx));
                });
            });
            return;
        case K::Slice:
            eval_paths(cx, *n.a, in, base, env, [&](const json& p, const json& v) {
                auto emit = [&](const json& lo, const json& hi) {
                    json np = p; np.push_back(json{{"start", lo}, {"end", hi}});
                    out(np, slice_value(v, lo, hi));
                };
                auto with_hi = [&](const json& lo) {
                    if (!n.c) { emit(lo, nullptr); return; }
                    eval(cx, *n.c, in, env, [&](const json& hi) { emit(lo, hi); });
                };
                if (!n.b) with_hi(nullptr); else eval(cx, *n.b, in, env, with_hi);
            });
            return;
        case K::Iterate:
            eval_paths(cx, *n.a, in, base, env, [&](const json& p, const json& v) {
                if (v.is_array()) {
                    for (std::size_t i = 0; i < v.size(); ++i) { json np = p; np.push_back(i); out(np, v[i]); }
                } else if (v.is_object()) {
                    for (auto it = v.begin(); it != v.end(); ++it) { json np = p; np.push_back(it.key()); out(np, *it); }
                } else if (!v.is_null()) {
                    fail("Cannot iterate over " + describe(v));
                }
            });
            return;
        case K::Pipe:
            eval_paths(cx, *n.a, in, base, env, [&](const json& p, const json& v) {
                eval_paths(cx, *n.b, v, p, env, out);
            });
            return;
        case K::Comma:
            eval_paths(cx, *n.a, in, base, env, out);
            eval_paths(cx, *n.b, in, base, env, out);
            return;
        case K::Try: {
            const auto id = cx.next_id++;
            const PathOut wrapped = [&](const json& p, const json& v) {
                try { out(p, v); } catch (JqError& e) { throw Downstream{id, std::move(e.value)}; }
            };
            try { eval_paths(cx, *n.a, in, base, env, wrapped); }
            catch (Downstream& d) { if (d.id != id) throw; throw JqError{std::move(d.value)}; }
            catch (JqError&) { if (n.b) fail("try/catch is not a path expression"); }
            return;
        }
        case K::Alt: {
            bool any = false;
            eval_paths(cx, *n.a, in, base, env, [&](const json& p, const json& v) {
                if (truthy(v)) { any = true; out(p, v); }
            });
            if (!any) eval_paths(cx, *n.b, in, base, env, out);
            return;
        }
        case K::If: {
            std::function<void(std::size_t)> branch = [&](std::size_t i) {
                if (i == n.args.size()) {
                    if (n.a) eval_paths(cx, *n.a, in, base, env, out); else out(base, in);
                    return;
                }
                eval(cx, *n.args[i], in, env, [&](const json& c) {
                    if (truthy(c)) eval_paths(cx, *n.args2[i], in, base, env, out);
                    else branch(i + 1);
                });
            };
            branch(0);
            return;
        }
        case K::As:
            eval(cx, *n.a, in, env, [&](const json& v) { eval_paths(cx, *n.b, in, base, bind(env, n.s, v), out); });
            return;
        case K::Literal:
            if (n.lit.is_null()) { out(base, nullptr); return; }   // `path(null)`, `getpath`-style no-op
            break;
        case K::Call:
            if (call_builtin_paths(cx, n, in, base, env, out)) return;
            break;
        default: break;
    }
    fail("Invalid path expression (" + std::string(n.k == K::Call ? n.s + "/" + std::to_string(n.args.size()) : "this filter")
         + " doesn't select a location in the input)");
}

} // namespace mcp::tools::util::jq::detail
