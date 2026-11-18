// jq_builtins.cpp — the builtin function library and @formats.
#include "jq_ast.hpp"

#include <mcp/tools/util/regex_guard.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <locale>
#include <regex>
#include <stdexcept>

namespace mcp::tools::util::jq::detail {
namespace {

using Args = std::vector<NodeP>;

// Evaluate arg `i` (a closure over the call's input) for every output.
void arg(Ctx& cx, const Node& c, std::size_t i, const json& in, const EnvP& env,
         const std::function<void(const json&)>& f) {
    eval(cx, *c.args[i], in, env, f);
}

json collect(Ctx& cx, const Node& n, const json& in, const EnvP& env) {
    json a = json::array();
    eval(cx, n, in, env, [&](const json& v) { a.push_back(v); });
    return a;
}

const std::string& need_str(const json& v, const char* fn) {
    if (!v.is_string()) fail(describe(v) + " cannot be used with " + fn + " (needs a string)");
    return v.get_ref<const std::string&>();
}
const json& need_arr(const json& v, const char* fn) {
    if (!v.is_array()) fail(std::string("Cannot use ") + fn + " on " + describe(v) + " (needs an array)");
    return v;
}

json length_of(const json& v) {
    if (v.is_null()) return 0;
    if (v.is_boolean()) fail(describe(v) + " has no length");
    if (v.is_number()) return num(std::fabs(to_double(v)));
    if (v.is_string()) return cp_len(v.get_ref<const std::string&>());
    return v.size();
}

json keys_of(const json& v, bool sorted) {
    json out = json::array();
    if (v.is_object()) {
        std::vector<std::string> k;
        for (auto it = v.begin(); it != v.end(); ++it) k.push_back(it.key());
        if (sorted) std::sort(k.begin(), k.end());
        for (auto& s : k) out.push_back(std::move(s));
    } else if (v.is_array()) {
        for (std::size_t i = 0; i < v.size(); ++i) out.push_back(i);
    } else {
        fail(describe(v) + " has no keys");
    }
    return out;
}

bool contains(const json& a, const json& b) {
    if (a.is_object() && b.is_object()) {
        for (auto it = b.begin(); it != b.end(); ++it) {
            auto ai = a.find(it.key());
            if (ai == a.end() || !contains(*ai, *it)) return false;
        }
        return true;
    }
    if (a.is_array() && b.is_array()) {
        for (const auto& x : b) {
            bool found = false;
            for (const auto& y : a) if (contains(y, x)) { found = true; break; }
            if (!found) return false;
        }
        return true;
    }
    if (a.is_string() && b.is_string())
        return a.get_ref<const std::string&>().find(b.get_ref<const std::string&>()) != std::string::npos;
    if (type_of(a) == type_of(b)) return equal(a, b);
    fail("An " + type_of(a) + " (not " + type_of(b) + ") cannot have its containment checked");
}

json flatten(const json& v, double depth) {
    if (depth < 0) fail("flatten depth must not be negative");
    json out = json::array();
    for (const auto& e : need_arr(v, "flatten")) {
        if (e.is_array() && depth > 0) for (const auto& x : flatten(e, depth - 1)) out.push_back(x);
        else out.push_back(e);
    }
    return out;
}

json sorted(json a) {
    std::stable_sort(a.begin(), a.end(), [](const json& x, const json& y) { return cmp(x, y) < 0; });
    return a;
}

json unique_sorted(const json& a) {
    json s = sorted(a), out = json::array();
    for (const auto& e : s) if (out.empty() || !equal(out.back(), e)) out.push_back(e);
    return out;
}

// Pairs of (key, element) with key = [outputs of f]; stable-sorted by key.
std::vector<std::pair<json, json>> keyed(Ctx& cx, const Node& f, const json& arr, const EnvP& env) {
    std::vector<std::pair<json, json>> v;
    for (const auto& e : need_arr(arr, "sort_by/group_by")) v.emplace_back(collect(cx, f, e, env), e);
    std::stable_sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return cmp(x.first, y.first) < 0; });
    return v;
}

json indices_of(const json& t, const json& x) {
    json out = json::array();
    if (t.is_null()) return nullptr;
    if (t.is_string()) {
        const auto& s = t.get_ref<const std::string&>();
        const auto& n = need_str(x, "indices");
        if (n.empty()) return nullptr;
        for (std::size_t p = s.find(n); p != std::string::npos; p = s.find(n, p + 1)) out.push_back(byte_to_cp(s, p));
        return out;
    }
    if (t.is_array()) {
        const json sub = x.is_array() ? x : json::array({x});
        if (sub.empty()) return nullptr;
        for (std::size_t i = 0; i + sub.size() <= t.size(); ++i) {
            bool m = true;
            for (std::size_t j = 0; j < sub.size() && m; ++j) m = equal(t[i + j], sub[j]);
            if (m) out.push_back(i);
        }
        return out;
    }
    fail("Cannot determine indices in " + describe(t));
}

// Regexes run on UTF-32 (std::wregex; wchar_t is 32-bit on the hosts we
// build for), so `.` is one CHARACTER like Oniguruma, not one byte of a
// multibyte sequence, and match offsets are code points as jq reports them.
static_assert(sizeof(wchar_t) == 4, "jq regex needs a 32-bit wchar_t");

std::wstring widen(std::string_view s) {
    std::wstring w;
    w.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto c0 = static_cast<unsigned char>(s[i]);
        int len = c0 < 0x80 ? 1 : (c0 >> 5) == 6 ? 2 : (c0 >> 4) == 14 ? 3 : (c0 >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + static_cast<std::size_t>(len) > s.size()) { w += L'\xFFFD'; ++i; continue; }
        std::uint32_t cp = len == 1 ? c0 : len == 2 ? (c0 & 0x1F) : len == 3 ? (c0 & 0x0F) : (c0 & 0x07);
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { w += L'\xFFFD'; ++i; continue; }
        w += static_cast<wchar_t>(cp);
        i += static_cast<std::size_t>(len);
    }
    return w;
}

std::string narrow(std::wstring_view w) {
    std::string o;
    o.reserve(w.size());
    for (wchar_t wc : w) {
        const auto cp = static_cast<std::uint32_t>(wc);
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    return o;
}

// ── regex ──────────────────────────────────────────────────────────────────
struct Re {
    std::wregex re;
    std::vector<std::string> names;   // per capture group; "" = unnamed
    bool global = false, skip_empty = false;
};

Re compile(const std::string& pat, const std::string& flags) {
    // std::regex backtracks without a time limit: refuse the classic
    // catastrophic shapes up front instead of hanging the tool call.
    if (util::has_nested_quantifier(pat))
        fail("regex \"" + pat + "\" has a nested unbounded quantifier like (a+)+, which "
             "can take forever to match; rewrite it without the nested +/*");
    if (pat.size() > 4096) fail("regex longer than 4096 bytes");
    Re r;
    auto f = std::regex::ECMAScript;
    std::string p;
    bool extended = false;
    for (char c : flags) {
        switch (c) {
            case 'g': r.global = true; break;
            case 'i': f |= std::regex::icase; break;
            case 'x': extended = true; break;
            case 'n': r.skip_empty = true; break;
            case 's': case 'l': case 'p': break;   // accepted, no ECMAScript equivalent
            default: fail(std::string("\"") + flags + "\" is not a valid modifier string");
        }
    }
    // Rewrite Oniguruma-style named groups `(?<name>...)` to plain groups and
    // remember the names; std::regex has no named groups.
    bool in_class = false;
    for (std::size_t i = 0; i < pat.size(); ++i) {
        const char c = pat[i];
        if (c == '\\' && i + 1 < pat.size()) { p += c; p += pat[++i]; continue; }
        if (extended && !in_class && (c == ' ' || c == '\t' || c == '\n')) continue;
        if (extended && !in_class && c == '#') { while (i < pat.size() && pat[i] != '\n') ++i; continue; }
        if (c == '[') in_class = true;
        else if (c == ']') in_class = false;
        if (c == '(' && !in_class) {
            if (i + 1 < pat.size() && pat[i + 1] == '?') {
                if (i + 2 < pat.size() && pat[i + 2] == '<' && i + 3 < pat.size() && pat[i + 3] != '=' && pat[i + 3] != '!') {
                    const auto close = pat.find('>', i + 3);
                    if (close == std::string::npos) fail("bad named group in regex");
                    r.names.push_back(pat.substr(i + 3, close - i - 3));
                    p += '(';
                    i = close;
                    continue;
                }
                // (?: (?= (?! — not capturing
            } else {
                r.names.emplace_back();
            }
        }
        p += c;
    }
    // A UTF-8 ctype locale makes \w, \d, \s and [[:alpha:]] Unicode-aware,
    // as they are in jq's Oniguruma ("été" is one word). Fall back to the
    // classic locale (ASCII classes) on hosts without one.
    static const std::locale kLoc = [] {
        for (const char* name : {"C.UTF-8", "C.utf8", "en_US.UTF-8", "en_US.utf8"}) {
            try { return std::locale(name); } catch (const std::runtime_error&) {}
        }
        return std::locale::classic();
    }();
    try {
        r.re.imbue(kLoc);
        r.re.assign(widen(p), f);
    }
    catch (const std::regex_error& e) { fail(std::string("invalid regex: ") + e.what()); }
    return r;
}

using WMatch = std::wsmatch;

json match_obj(const WMatch& m, const Re& r) {
    json o{{"offset", m.position(0)}, {"length", m.length(0)}, {"string", narrow(m.str(0))}};
    json caps = json::array();
    for (std::size_t i = 1; i < m.size(); ++i) {
        json c;
        if (m[i].matched) c = {{"offset", m.position(i)}, {"length", m.length(i)}, {"string", narrow(m.str(i))}};
        else c = {{"offset", -1}, {"length", 0}, {"string", nullptr}};
        c["name"] = (i - 1 < r.names.size() && !r.names[i - 1].empty()) ? json(r.names[i - 1]) : json(nullptr);
        caps.push_back(std::move(c));
    }
    o["captures"] = std::move(caps);
    return o;
}

template <class F>
void for_matches(const std::wstring& s, const Re& r, F&& f) {
    if (s.size() > (4u << 20)) throw Fatal{"string too long for regex (4M characters cap)"};
    try {
        auto begin = std::wsregex_iterator(s.begin(), s.end(), r.re);
        for (auto it = begin; it != std::wsregex_iterator(); ++it) {
            if (r.skip_empty && it->length(0) == 0) continue;
            f(*it);
            if (!r.global) break;
        }
    } catch (const std::regex_error& e) {
        fail(std::string("regex failed (pattern too complex for this input?): ") + e.what());
    }
}

json capture_obj(const json& m) {
    json o = json::object();
    for (const auto& c : m["captures"]) if (!c["name"].is_null()) o[c["name"].get<std::string>()] = c["string"];
    return o;
}

std::string flags_arg(Ctx& cx, const Node& c, std::size_t i, const json& in, const EnvP& env) {
    if (c.args.size() <= i) return {};
    std::string f;
    arg(cx, c, i, in, env, [&](const json& v) { if (!v.is_null()) f = need_str(v, "regex flags"); });
    return f;
}

void regex_fn(Ctx& cx, const Node& c, const json& in, const EnvP& env, const Out& out) {
    const std::string& fn = c.s;
    const std::wstring s = widen(need_str(in, fn.c_str()));
    // The regex arg may itself be [re, flags].
    arg(cx, c, 0, in, env, [&](const json& rv) {
        std::string pat, flags;
        if (rv.is_array()) { pat = need_str(rv.at(0), fn.c_str()); if (rv.size() > 1 && !rv[1].is_null()) flags = need_str(rv[1], fn.c_str()); }
        else pat = need_str(rv, fn.c_str());
        const std::size_t flag_idx = (fn == "sub" || fn == "gsub") ? 2 : 1;
        flags += flags_arg(cx, c, flag_idx, in, env);
        if (fn == "gsub" || fn == "scan" || fn == "splits" || fn == "split") flags += 'g';
        const Re r = compile(pat, flags);
        auto sub = [&](std::size_t a, std::size_t b) { return narrow(std::wstring_view(s).substr(a, b - a)); };
        if (fn == "test") {
            bool hit = false;
            for_matches(s, r, [&](const WMatch&) { hit = true; });
            out(hit);
        } else if (fn == "match") {
            for_matches(s, r, [&](const WMatch& m) { out(match_obj(m, r)); });
        } else if (fn == "capture") {
            for_matches(s, r, [&](const WMatch& m) { out(capture_obj(match_obj(m, r))); });
        } else if (fn == "scan") {
            for_matches(s, r, [&](const WMatch& m) {
                if (m.size() == 1) out(narrow(m.str(0)));
                else { json a = json::array(); for (std::size_t i = 1; i < m.size(); ++i) a.push_back(m[i].matched ? json(narrow(m.str(i))) : json(nullptr)); out(a); }
            });
        } else if (fn == "splits" || fn == "split") {
            json parts = json::array();
            std::size_t prev = 0;
            for_matches(s, r, [&](const WMatch& m) {
                const auto p = static_cast<std::size_t>(m.position(0));
                parts.push_back(sub(prev, p));
                prev = p + static_cast<std::size_t>(m.length(0));
            });
            parts.push_back(sub(prev, s.size()));
            if (fn == "split") out(parts); else for (const auto& p : parts) out(p);
        } else {   // sub / gsub
            std::string result;
            std::size_t prev = 0;
            bool failed = false;
            for_matches(s, r, [&](const WMatch& m) {
                const auto p = static_cast<std::size_t>(m.position(0));
                result += sub(prev, p);
                const json caps = capture_obj(match_obj(m, r));
                bool got = false;
                eval(cx, *c.args[1], caps, env, [&](const json& rep) {
                    if (got) return;   // first output only
                    if (!rep.is_string()) { failed = true; return; }
                    result += rep.get<std::string>();
                    got = true;
                });
                prev = p + static_cast<std::size_t>(m.length(0));
            });
            if (failed) fail(fn + " replacement must be a string");
            result += sub(prev, s.size());
            out(result);
        }
    });
}

// ── formats ────────────────────────────────────────────────────────────────
std::string csv_cell(const json& v, bool tsv) {
    if (v.is_null()) return "";
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number()) return v.dump();
    if (!v.is_string()) fail(describe(v) + " is not valid in a csv row");
    const auto& s = v.get_ref<const std::string&>();
    std::string o;
    if (tsv) {
        for (char c : s) {
            if (c == '\t') o += "\\t"; else if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r";
            else if (c == '\\') o += "\\\\"; else o += c;
        }
        return o;
    }
    o = "\"";
    for (char c : s) { if (c == '"') o += "\"\""; else o += c; }
    return o + "\"";
}

std::string base64(std::string_view in) {
    static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
        o += tbl[v >> 18]; o += tbl[(v >> 12) & 63]; o += tbl[(v >> 6) & 63]; o += tbl[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        o += tbl[v >> 18]; o += tbl[(v >> 12) & 63]; o += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        o += tbl[v >> 18]; o += tbl[(v >> 12) & 63]; o += tbl[(v >> 6) & 63]; o += '=';
    }
    return o;
}

std::string base64d(std::string_view in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string o;
    unsigned buf = 0; int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) { if (c == '\n' || c == '\r' || c == ' ') continue; fail("string is not valid base64"); }
        buf = (buf << 6) | static_cast<unsigned>(v); bits += 6;
        if (bits >= 8) { bits -= 8; o += static_cast<char>((buf >> bits) & 0xFF); }
    }
    return o;
}

} // namespace

std::string apply_format(const std::string& fmt, const json& v) {
    if (fmt == "text") return tostring(v);
    if (fmt == "json") return v.dump(-1, ' ', false, json::error_handler_t::replace);
    if (fmt == "csv" || fmt == "tsv") {
        if (!v.is_array()) fail(describe(v) + " cannot be " + fmt + "-formatted, only an array can be");
        std::string o;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) o += fmt == "csv" ? "," : "\t";
            o += csv_cell(v[i], fmt == "tsv");
        }
        return o;
    }
    if (fmt == "html") {
        std::string o;
        for (char c : tostring(v)) {
            switch (c) {
                case '<': o += "&lt;"; break; case '>': o += "&gt;"; break; case '&': o += "&amp;"; break;
                case '\'': o += "&apos;"; break; case '"': o += "&quot;"; break; default: o += c;
            }
        }
        return o;
    }
    if (fmt == "uri") {
        std::string o;
        static constexpr char hex[] = "0123456789ABCDEF";
        for (unsigned char c : tostring(v)) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
            else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
        }
        return o;
    }
    if (fmt == "sh") {
        auto quote = [](const json& x) -> std::string {
            if (x.is_array() || x.is_object()) fail(describe(x) + " can not be escaped for shell");
            if (!x.is_string()) return x.dump();
            std::string o = "'";
            for (char c : x.get<std::string>()) { if (c == '\'') o += "'\\''"; else o += c; }
            return o + "'";
        };
        if (!v.is_array()) return quote(v);
        std::string o;
        for (std::size_t i = 0; i < v.size(); ++i) { if (i) o += ' '; o += quote(v[i]); }
        return o;
    }
    if (fmt == "base64")  return base64(tostring(v));
    if (fmt == "base64d") return base64d(tostring(v));
    fail(fmt + " is not a valid format");
}

bool call_builtin(Ctx& cx, const Node& c, const json& in, const EnvP& env, const Out& out) {
    const std::string& f = c.s;
    const std::size_t n = c.args.size();
    auto is = [&](const char* name, std::size_t arity) { return f == name && n == arity; };

    // ── 0-arity ──
    if (n == 0) {
        if (f == "empty") return true;
        if (f == "not") { out(!truthy(in)); return true; }
        if (f == "length") { out(length_of(in)); return true; }
        if (f == "utf8bytelength") { out(need_str(in, "utf8bytelength").size()); return true; }
        if (f == "keys") { out(keys_of(in, true)); return true; }
        if (f == "keys_unsorted") { out(keys_of(in, false)); return true; }
        if (f == "type") { out(type_of(in)); return true; }
        if (f == "error") { throw JqError{in}; }
        if (f == "add") {
            if (in.is_null()) { out(nullptr); return true; }
            if (!in.is_array() && !in.is_object()) fail("Cannot iterate over " + describe(in));
            json acc = nullptr;
            for (const auto& e : in) acc = binop("+", acc, e);
            out(acc); return true;
        }
        if (f == "any" || f == "all") {
            const bool want_any = f == "any";
            bool r = !want_any;
            for (const auto& e : need_arr(in, f.c_str())) if (truthy(e) == want_any) { r = want_any; break; }
            out(r); return true;
        }
        if (f == "flatten") { out(flatten(in, 1e9)); return true; }
        if (f == "floor" || f == "ceil" || f == "round" || f == "sqrt" || f == "fabs" || f == "abs"
            || f == "log" || f == "log2" || f == "log10" || f == "exp" || f == "trunc") {
            if (!in.is_number()) fail(describe(in) + " number required");
            const double d = to_double(in);
            double r = f == "floor" ? std::floor(d) : f == "ceil" ? std::ceil(d) : f == "round" ? std::round(d)
                     : f == "sqrt" ? std::sqrt(d) : (f == "fabs" || f == "abs") ? std::fabs(d)
                     : f == "log" ? std::log(d) : f == "log2" ? std::log2(d) : f == "log10" ? std::log10(d)
                     : f == "exp" ? std::exp(d) : std::trunc(d);
            out(num(r)); return true;
        }
        if (f == "tostring") { out(tostring(in)); return true; }
        if (f == "tonumber") {
            if (in.is_number()) { out(in); return true; }
            const auto& s = need_str(in, "tonumber");
            char* end = nullptr;
            const double d = std::strtod(s.c_str(), &end);
            if (s.empty() || end != s.c_str() + s.size() || std::isspace(static_cast<unsigned char>(s[0])))
                fail("Cannot parse '" + s + "' as a number");
            out(num(d)); return true;
        }
        if (f == "ascii_downcase" || f == "ascii_upcase") {
            std::string s = need_str(in, f.c_str());
            for (auto& ch : s) ch = static_cast<char>(f == "ascii_downcase" ? std::tolower(static_cast<unsigned char>(ch)) : std::toupper(static_cast<unsigned char>(ch)));
            out(s); return true;
        }
        if (f == "trim" || f == "ltrim" || f == "rtrim") {
            const auto& s = need_str(in, f.c_str());
            std::size_t a = 0, b = s.size();
            if (f != "rtrim") while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
            if (f != "ltrim") while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
            out(s.substr(a, b - a)); return true;
        }
        if (f == "explode") {
            json o = json::array();
            const auto& s = need_str(in, "explode");
            for (std::size_t i = 0; i < s.size();) {
                const auto c0 = static_cast<unsigned char>(s[i]);
                const int len = c0 < 0x80 ? 1 : c0 < 0xE0 ? 2 : c0 < 0xF0 ? 3 : 4;
                std::uint32_t cp = len == 1 ? c0 : len == 2 ? (c0 & 0x1F) : len == 3 ? (c0 & 0x0F) : (c0 & 0x07);
                for (int k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
                o.push_back(cp); i += len;
            }
            out(o); return true;
        }
        if (f == "implode") {
            std::string s;
            for (const auto& cpv : need_arr(in, "implode")) {
                if (!cpv.is_number()) fail("Unicode codepoint must be numeric");
                const auto cp = static_cast<std::uint32_t>(to_double(cpv));
                if (cp < 0x80) s += static_cast<char>(cp);
                else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
                else { s += static_cast<char>(0xF0 | (cp >> 18)); s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
            }
            out(s); return true;
        }
        if (f == "tojson") { out(in.dump(-1, ' ', false, json::error_handler_t::replace)); return true; }
        if (f == "have_literal_numbers" || f == "have_decnum") { out(f == "have_literal_numbers"); return true; }
        if (f == "fromjson") {
            try { out(json::parse(need_str(in, "fromjson"))); }
            catch (const json::parse_error& e) { fail(std::string(need_str(in, "fromjson")) + " (while parsing): " + e.what()); }
            return true;
        }
        if (f == "to_entries") {
            json o = json::array();
            for (const auto& k : keys_of(in, false))
                o.push_back({{"key", k}, {"value", index_value(in, k)}});
            out(o); return true;
        }
        if (f == "from_entries") {
            json o = json::object();
            for (const auto& e : need_arr(in, "from_entries")) {
                if (!e.is_object()) fail("from_entries needs objects with key/value");
                json k = nullptr;
                for (const char* kn : {"key", "k", "name", "Name", "Key", "K"}) if (auto it = e.find(kn); it != e.end() && !it->is_null()) { k = *it; break; }
                json v = nullptr;
                for (const char* vn : {"value", "v", "Value", "V"}) if (auto it = e.find(vn); it != e.end()) { v = *it; break; }
                if (k.is_null() || k.is_object() || k.is_array()) fail("Cannot use " + describe(k) + " as object key");
                o[k.is_string() ? k.get<std::string>() : tostring(k)] = v;
            }
            out(o); return true;
        }
        if (f == "sort") { out(sorted(need_arr(in, "sort"))); return true; }
        if (f == "unique") { out(unique_sorted(need_arr(in, "unique"))); return true; }
        if (f == "reverse") {
            if (in.is_null()) { out(json::array()); return true; }
            if (in.is_string()) fail("Cannot index string with number (" + std::to_string(cp_len(in.get_ref<const std::string&>()) - 1) + ")");
            json a = need_arr(in, "reverse"); std::reverse(a.begin(), a.end()); out(a); return true;
        }
        if (f == "min" || f == "max") {
            const auto& a = need_arr(in, f.c_str());
            if (a.empty()) { out(nullptr); return true; }
            const json* best = &a[0];
            for (const auto& e : a) if (f == "min" ? cmp(e, *best) < 0 : cmp(e, *best) >= 0) best = &e;
            out(*best); return true;
        }
        if (f == "first") { out(index_value(in, 0)); return true; }
        if (f == "last")  { out(index_value(in, -1)); return true; }
        if (f == "recurse") { Node r; r.k = K::Recurse; eval(cx, r, in, env, out); return true; }
        if (f == "paths" || f == "leaf_paths") {
            Node rec; rec.k = K::Recurse;
            eval_paths(cx, rec, in, json::array(), env, [&](const json& p, const json& v) {
                if (p.empty()) return;
                if (f == "leaf_paths" && (v.is_array() || v.is_object())) return;
                out(p);
            });
            return true;
        }
        if (f == "env") { out(json::object()); return true; }
        if (f == "input_filename") { out(nullptr); return true; }
        if (f == "debug" || f == "stderr") { out(in); return true; }
        if (f == "input" || f == "inputs") fail(f + " is not supported (json_query has a single input)");
        if (f == "now" || f == "todate" || f == "fromdate" || f == "strftime" || f == "mktime" || f == "gmtime")
            fail(f + " (date functions) is not supported");
        if (f == "nulls" || f == "booleans" || f == "numbers" || f == "strings" || f == "arrays" || f == "objects"
            || f == "iterables" || f == "scalars" || f == "values") {
            const std::string t = type_of(in);
            const bool keep = f == "values" ? !in.is_null()
                            : f == "iterables" ? (t == "array" || t == "object")
                            : f == "scalars" ? !(t == "array" || t == "object")
                            : (f == "nulls" && t == "null") || (f == "booleans" && t == "boolean") || (f == "numbers" && t == "number")
                              || (f == "strings" && t == "string") || (f == "arrays" && t == "array") || (f == "objects" && t == "object");
            if (keep) out(in);
            return true;
        }
        if (f == "transpose") {
            const auto& a = need_arr(in, "transpose");
            std::size_t w = 0;
            for (const auto& r : a) w = std::max(w, need_arr(r, "transpose").size());
            json o = json::array();
            for (std::size_t i = 0; i < w; ++i) { json row = json::array(); for (const auto& r : a) row.push_back(i < r.size() ? r[i] : json(nullptr)); o.push_back(row); }
            out(o); return true;
        }
        if (f == "toarray") { out(in.is_array() ? in : json::array({in})); return true; }
        if (f == "infinite") { out(num(INFINITY)); return true; }
        if (f == "nan") { out(nullptr); return true; }
        if (f == "isinfinite" || f == "isnan" || f == "isnormal") {
            if (!in.is_number()) fail(describe(in) + " number required");
            const double d = to_double(in);
            out(f == "isinfinite" ? std::isinf(d) : f == "isnan" ? std::isnan(d) : std::isnormal(d)); return true;
        }
        if (f == "ascii") {
            if (!in.is_number()) fail("ascii needs a number");
            out(std::string(1, static_cast<char>(to_double(in)))); return true;
        }
        if (f == "halt" || f == "halt_error") fail(f + " is not supported");
        if (f != "combinations") return false;
    }

    // ── 1+-arity ──
    if (is("error", 1)) { arg(cx, c, 0, in, env, [&](const json& m) { throw JqError{m}; }); return true; }
    if (is("select", 1)) { arg(cx, c, 0, in, env, [&](const json& v) { if (truthy(v)) out(in); }); return true; }
    if (is("map", 1)) {
        json o = json::array();
        if (!in.is_array() && !in.is_object()) fail("Cannot iterate over " + describe(in));
        for (const auto& e : in) eval(cx, *c.args[0], e, env, [&](const json& v) { o.push_back(v); });
        out(o); return true;
    }
    if (is("map_values", 1)) {
        json o = in;
        if (in.is_object()) {
            for (auto it = in.begin(); it != in.end(); ++it) {
                bool got = false; json nv;
                const auto id = cx.next_id++;
                try { eval(cx, *c.args[0], *it, env, [&](const json& v) { nv = v; got = true; throw Stop{id}; }); }
                catch (Stop& s) { if (s.id != id) throw; }
                if (got) o[it.key()] = nv; else o.erase(it.key());
            }
        } else if (in.is_array()) {
            json a = json::array();
            for (const auto& e : in) {
                const auto id = cx.next_id++;
                try { eval(cx, *c.args[0], e, env, [&](const json& v) { a.push_back(v); throw Stop{id}; }); }
                catch (Stop& s) { if (s.id != id) throw; }
            }
            o = a;
        } else fail("Cannot iterate over " + describe(in));
        out(o); return true;
    }
    if (is("with_entries", 1)) {
        if (!in.is_object()) fail(describe(in) + " has no keys");
        json entries = json::array();
        for (auto it = in.begin(); it != in.end(); ++it) entries.push_back({{"key", it.key()}, {"value", *it}});
        json mapped = json::array();
        for (const auto& e : entries) eval(cx, *c.args[0], e, env, [&](const json& v) { mapped.push_back(v); });
        Node fe; fe.k = K::Call; fe.s = "from_entries";
        return call_builtin(cx, fe, mapped, env, out);
    }
    if (is("walk", 1)) {
        std::function<json(const json&)> w = [&](const json& v) -> json {
            DepthGuard g(cx);
            json x = v;
            if (v.is_object()) { for (auto it = x.begin(); it != x.end(); ++it) *it = w(*it); }
            else if (v.is_array()) { for (auto& e : x) e = w(e); }
            json last = nullptr; bool got = false;
            eval(cx, *c.args[0], x, env, [&](const json& r) { last = r; got = true; });
            return got ? last : json(nullptr);
        };
        out(w(in)); return true;
    }
    if (is("recurse", 1) || is("recurse", 2)) {
        // def recurse(f; cond): def r: ., (f | select(cond) | r); r;
        std::function<void(const json&)> r = [&](const json& v) {
            DepthGuard g(cx);
            out(v);
            eval(cx, *c.args[0], v, env, [&](const json& next) {
                if (n == 1) { r(next); return; }
                eval(cx, *c.args[1], next, env, [&](const json& t) { if (truthy(t)) r(next); });
            });
        };
        r(in); return true;
    }
    if (is("has", 1)) {
        arg(cx, c, 0, in, env, [&](const json& k) {
            if (in.is_object() && k.is_string()) out(in.contains(k.get<std::string>()));
            else if (in.is_array() && k.is_number()) { const double d = to_double(k); out(d >= 0 && d < static_cast<double>(in.size())); }
            else fail("Cannot check whether " + type_of(in) + " has a " + type_of(k) + " key");
        });
        return true;
    }
    if (is("in", 1)) {
        arg(cx, c, 0, in, env, [&](const json& o) {
            Node h; h.k = K::Call; h.s = "has"; auto lit = std::make_shared<Node>(); lit->k = K::Literal; lit->lit = in; h.args = {lit};
            call_builtin(cx, h, o, env, out);
        });
        return true;
    }
    if (is("contains", 1)) { arg(cx, c, 0, in, env, [&](const json& b) { out(contains(in, b)); }); return true; }
    if (is("inside", 1))   { arg(cx, c, 0, in, env, [&](const json& b) { out(contains(b, in)); }); return true; }
    if (is("startswith", 1) || is("endswith", 1) || is("ltrimstr", 1) || is("rtrimstr", 1)) {
        arg(cx, c, 0, in, env, [&](const json& x) {
            if (f == "ltrimstr" || f == "rtrimstr") {
                if (!in.is_string() || !x.is_string()) fail("startswith() requires string inputs");
                const auto& s = in.get_ref<const std::string&>(); const auto& p = x.get_ref<const std::string&>();
                if (f == "ltrimstr") out(s.starts_with(p) ? s.substr(p.size()) : s);
                else out(s.ends_with(p) ? s.substr(0, s.size() - p.size()) : s);
                return;
            }
            const auto& s = need_str(in, f.c_str());
            const auto& p = need_str(x, f.c_str());
            out(f == "startswith" ? s.starts_with(p) : s.ends_with(p));
        });
        return true;
    }
    if (is("split", 1)) {
        arg(cx, c, 0, in, env, [&](const json& sep) { out(binop("/", json(need_str(in, "split")), json(need_str(sep, "split")))); });
        return true;
    }
    if (is("join", 1)) {
        arg(cx, c, 0, in, env, [&](const json& sep) {
            const auto& s = need_str(sep, "join");
            std::string o;
            bool first = true;
            for (const auto& e : need_arr(in, "join")) {
                if (!first) o += s;
                first = false;
                if (e.is_null()) continue;
                if (e.is_array() || e.is_object()) fail("Cannot join with " + describe(e));
                o += tostring(e);
            }
            out(o);
        });
        return true;
    }
    if (f == "test" || f == "match" || f == "capture" || f == "scan" || f == "splits"
        || (f == "split" && n == 2) || f == "sub" || f == "gsub") {
        const std::size_t min_n = (f == "sub" || f == "gsub") ? 2 : 1;
        if (n < min_n || n > min_n + 1) return false;
        regex_fn(cx, c, in, env, out);
        return true;
    }
    if (is("indices", 1) || is("index", 1) || is("rindex", 1)) {
        arg(cx, c, 0, in, env, [&](const json& x) {
            const json idx = indices_of(in, x);
            if (f == "indices") out(idx);
            else if (idx.is_null() || idx.empty()) out(nullptr);
            else out(f == "index" ? idx.front() : idx.back());
        });
        return true;
    }
    if (is("flatten", 1)) {
        arg(cx, c, 0, in, env, [&](const json& d) { if (!d.is_number()) fail("flatten depth must be a number"); out(flatten(in, to_double(d))); });
        return true;
    }
    if (is("sort_by", 1)) {
        json o = json::array();
        for (auto& [k, e] : keyed(cx, *c.args[0], in, env)) o.push_back(e);
        out(o); return true;
    }
    if (is("group_by", 1) || is("unique_by", 1)) {
        json o = json::array();
        const json* prev = nullptr;
        auto ks = keyed(cx, *c.args[0], in, env);
        for (auto& [k, e] : ks) {
            const bool same = prev && equal(*prev, k);
            if (f == "group_by") { if (!same) o.push_back(json::array()); o.back().push_back(e); }
            else if (!same) o.push_back(e);
            prev = &k;
        }
        out(o); return true;
    }
    if (is("min_by", 1) || is("max_by", 1)) {
        auto ks = keyed(cx, *c.args[0], in, env);
        if (ks.empty()) { out(nullptr); return true; }
        out(f == "min_by" ? ks.front().second : ks.back().second); return true;
    }
    if (is("any", 1) || is("all", 1)) {
        const bool want_any = f == "any";
        bool r = !want_any;
        for (const auto& e : need_arr(in, f.c_str())) {
            bool t = false;
            eval(cx, *c.args[0], e, env, [&](const json& v) { t = t || truthy(v); });
            if (t == want_any) { r = want_any; break; }
        }
        out(r); return true;
    }
    if (is("any", 2) || is("all", 2)) {
        const bool want_any = f == "any";
        const auto id = cx.next_id++;
        bool r = !want_any;
        try {
            eval(cx, *c.args[0], in, env, [&](const json& g) {
                eval(cx, *c.args[1], g, env, [&](const json& v) {
                    if (truthy(v) == want_any) { r = want_any; throw Stop{id}; }
                });
            });
        } catch (Stop& s) { if (s.id != id) throw; }
        out(r); return true;
    }
    if (is("IN", 1)) {
        const auto id = cx.next_id++;
        bool r = false;
        try { arg(cx, c, 0, in, env, [&](const json& v) { if (equal(v, in)) { r = true; throw Stop{id}; } }); }
        catch (Stop& s) { if (s.id != id) throw; }
        out(r); return true;
    }
    if (is("add", 1)) {
        json acc = nullptr;
        arg(cx, c, 0, in, env, [&](const json& v) { acc = binop("+", acc, v); });
        out(acc); return true;
    }
    if (is("range", 1) || is("range", 2) || is("range", 3)) {
        auto run = [&](double from, double upto, double by) {
            if (by == 0) return;   // jq 1.8: a zero step yields nothing
            for (double x = from; by > 0 ? x < upto : x > upto; x += by) { tick(cx); out(num(x)); }
        };
        auto need_num = [](const json& v) { if (!v.is_number()) fail("range bounds must be numbers"); return to_double(v); };
        if (n == 1) arg(cx, c, 0, in, env, [&](const json& u) { run(0, need_num(u), 1); });
        else arg(cx, c, 0, in, env, [&](const json& a) {
            arg(cx, c, 1, in, env, [&](const json& b) {
                if (n == 2) run(need_num(a), need_num(b), 1);
                else arg(cx, c, 2, in, env, [&](const json& s) { run(need_num(a), need_num(b), need_num(s)); });
            });
        });
        return true;
    }
    if (is("limit", 2)) {
        arg(cx, c, 0, in, env, [&](const json& lim) {
            if (!lim.is_number()) fail("limit needs a number");
            const double max = to_double(lim);
            if (max <= 0) return;
            const auto id = cx.next_id++;
            double seen = 0;
            try { arg(cx, c, 1, in, env, [&](const json& v) { out(v); if (++seen >= max) throw Stop{id}; }); }
            catch (Stop& s) { if (s.id != id) throw; }
        });
        return true;
    }
    if (is("first", 1)) {
        const auto id = cx.next_id++;
        try { arg(cx, c, 0, in, env, [&](const json& v) { out(v); throw Stop{id}; }); }
        catch (Stop& s) { if (s.id != id) throw; }
        return true;
    }
    if (is("last", 1)) {
        bool got = false; json last;
        arg(cx, c, 0, in, env, [&](const json& v) { last = v; got = true; });
        if (got) out(last);
        return true;
    }
    if (is("nth", 1)) { arg(cx, c, 0, in, env, [&](const json& i) { out(index_value(in, i)); }); return true; }
    if (is("nth", 2)) {
        arg(cx, c, 0, in, env, [&](const json& iv) {
            if (!iv.is_number() || to_double(iv) < 0) fail("nth needs a non-negative number");
            const double want = to_double(iv);
            const auto id = cx.next_id++;
            double k = 0;
            try { arg(cx, c, 1, in, env, [&](const json& v) { if (k++ == want) { out(v); throw Stop{id}; } }); }
            catch (Stop& s) { if (s.id != id) throw; }
        });
        return true;
    }
    if (is("isempty", 1)) {
        const auto id = cx.next_id++;
        bool empty = true;
        try { arg(cx, c, 0, in, env, [&](const json&) { empty = false; throw Stop{id}; }); }
        catch (Stop& s) { if (s.id != id) throw; }
        out(empty); return true;
    }
    if (is("until", 2)) {
        // def until(cond; update): def _until: if cond then . else (update | _until) end; _until;
        std::function<void(const json&)> u = [&](const json& v) {
            DepthGuard g(cx);
            eval(cx, *c.args[0], v, env, [&](const json& t) {
                if (truthy(t)) out(v);
                else eval(cx, *c.args[1], v, env, u);
            });
        };
        u(in); return true;
    }
    if (is("while", 2)) {
        // def while(cond; update): def _while: if cond then ., (update | _while) else empty end; _while;
        std::function<void(const json&)> w = [&](const json& v) {
            DepthGuard g(cx);
            eval(cx, *c.args[0], v, env, [&](const json& t) {
                if (!truthy(t)) return;
                out(v);
                eval(cx, *c.args[1], v, env, w);
            });
        };
        w(in); return true;
    }
    if (is("path", 1)) {
        eval_paths(cx, *c.args[0], in, json::array(), env, [&](const json& p, const json&) { out(p); });
        return true;
    }
    if (is("paths", 1)) {
        // def paths(f): . as $in | paths | select(. as $p | $in | getpath($p) | f);
        Node rec; rec.k = K::Recurse;
        eval_paths(cx, rec, in, json::array(), env, [&](const json& p, const json& v) {
            if (p.empty()) return;
            eval(cx, *c.args[0], v, env, [&](const json& t) { if (truthy(t)) out(p); });
        });
        return true;
    }
    if (is("INDEX", 1) || is("INDEX", 2)) {
        // def INDEX(stream; idx): reduce stream as $row ({}; .[$row|idx|tostring] |= $row);
        // def INDEX(idx): INDEX(.[]; idx);
        json acc = json::object();
        const Node& key_f = *c.args[n - 1];
        auto add_row = [&](const json& row) {
            eval(cx, key_f, row, env, [&](const json& k) { acc[tostring(k)] = row; });
        };
        if (n == 1) {
            if (!in.is_array() && !in.is_object()) fail("Cannot iterate over " + describe(in));
            for (const auto& row : in) add_row(row);
        } else {
            arg(cx, c, 0, in, env, add_row);
        }
        out(acc); return true;
    }
    if (is("IN", 2)) {
        // def IN(src; s): any(src == s; .);
        const auto id = cx.next_id++;
        bool r = false;
        try {
            arg(cx, c, 0, in, env, [&](const json& a) {
                arg(cx, c, 1, in, env, [&](const json& b) { if (equal(a, b)) { r = true; throw Stop{id}; } });
            });
        } catch (Stop& s) { if (s.id != id) throw; }
        out(r); return true;
    }
    if (is("repeat", 1)) {
        // jq 1.8: repeat(f) emits f applied to the SAME input, forever (use
        // limit/first to bound it; the step budget catches the rest).
        for (;;) { tick(cx); arg(cx, c, 0, in, env, out); }
    }
    if (is("combinations", 0) || is("combinations", 1)) {
        json sets;
        if (n == 0) sets = need_arr(in, "combinations");
        else arg(cx, c, 0, in, env, [&](const json& k) {
            if (!k.is_number() || to_double(k) < 0) fail("combinations needs a non-negative count");
            sets = json::array();
            for (long long i = 0; i < static_cast<long long>(to_double(k)); ++i) sets.push_back(need_arr(in, "combinations"));
        });
        for (const auto& s : sets) need_arr(s, "combinations");
        std::function<void(std::size_t, json&)> go = [&](std::size_t i, json& acc) {
            tick(cx);
            if (i == sets.size()) { out(acc); return; }
            for (const auto& e : sets[i]) { acc.push_back(e); go(i + 1, acc); acc.erase(acc.size() - 1); }
        };
        json acc = json::array();
        go(0, acc);
        return true;
    }
    if (is("pick", 1)) {
        // def pick(pathexps): . as $top | reduce path(pathexps) as $p (null; setpath($p; $top | getpath($p)));
        json acc = nullptr;
        eval_paths(cx, *c.args[0], in, json::array(), env, [&](const json& p, const json&) {
            setpath(acc, p, 0, getpath(in, p));
        });
        out(acc); return true;
    }
    if (is("debug", 1)) { out(in); return true; }
    if (is("getpath", 1)) {
        arg(cx, c, 0, in, env, [&](const json& p) {
            if (!p.is_array()) fail("Path must be specified as an array");
            try { out(getpath(in, p)); } catch (JqError&) { out(nullptr); }
        });
        return true;
    }
    if (is("setpath", 2)) {
        arg(cx, c, 1, in, env, [&](const json& v) {
            arg(cx, c, 0, in, env, [&](const json& p) {
                if (!p.is_array()) fail("Path must be specified as an array");
                json r = in; setpath(r, p, 0, v); out(r);
            });
        });
        return true;
    }
    if (is("delpaths", 1)) { arg(cx, c, 0, in, env, [&](const json& ps) { out(delpaths(in, ps)); }); return true; }
    if (is("del", 1)) {
        json ps = json::array();
        eval_paths(cx, *c.args[0], in, json::array(), env, [&](const json& p, const json&) { ps.push_back(p); });
        out(delpaths(in, ps)); return true;
    }
    if (is("pow", 2)) {
        arg(cx, c, 1, in, env, [&](const json& b) {
            arg(cx, c, 0, in, env, [&](const json& a) {
                if (!a.is_number() || !b.is_number()) fail("pow needs numbers");
                out(num(std::pow(to_double(a), to_double(b))));
            });
        });
        return true;
    }
    return false;
}

bool call_builtin_paths(Ctx& cx, const Node& c, const json& in, const json& base,
                        const EnvP& env, const PathOut& out) {
    const std::string& f = c.s;
    const std::size_t n = c.args.size();
    if (f == "empty" && n == 0) return true;
    if (f == "error") { call_builtin(cx, c, in, env, [](const json&) {}); return true; }
    if (f == "select" && n == 1) {
        eval(cx, *c.args[0], in, env, [&](const json& v) { if (truthy(v)) out(base, in); });
        return true;
    }
    if (f == "recurse" && n == 0) {
        Node rec; rec.k = K::Recurse;
        eval_paths(cx, rec, in, base, env, out);
        return true;
    }
    if ((f == "first" || f == "last") && n == 0) {
        if (!in.is_array() && !in.is_null()) fail("Cannot index " + type_of(in) + " with number");
        json p = base;
        long long i = f == "first" ? 0 : static_cast<long long>(in.is_array() ? in.size() : 0) - 1;
        p.push_back(i < 0 ? -1 : i);
        out(p, index_value(in, f == "first" ? 0 : -1));
        return true;
    }
    if (f == "getpath" && n == 1) {
        eval(cx, *c.args[0], in, env, [&](const json& p) {
            if (!p.is_array()) fail("Path must be specified as an array");
            json np = base; for (const auto& k : p) np.push_back(k);
            json v; try { v = getpath(in, p); } catch (JqError&) { v = nullptr; }
            out(np, v);
        });
        return true;
    }
    if ((f == "first" && n == 1) || (f == "limit" && n == 2)) {
        const NodeP& g = f == "first" ? c.args[0] : c.args[1];
        auto run = [&](double max) {
            if (max <= 0) return;
            const auto id = cx.next_id++;
            double seen = 0;
            try { eval_paths(cx, *g, in, base, env, [&](const json& p, const json& v) { out(p, v); if (++seen >= max) throw Stop{id}; }); }
            catch (Stop& s) { if (s.id != id) throw; }
        };
        if (f == "first") run(1);
        else eval(cx, *c.args[0], in, env, [&](const json& m) { if (!m.is_number()) fail("limit needs a number"); run(to_double(m)); });
        return true;
    }
    return false;
}

} // namespace mcp::tools::util::jq::detail
