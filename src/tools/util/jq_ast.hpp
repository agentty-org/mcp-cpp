// jq_ast.hpp — internal to jq.cpp/jq_*.cpp: the AST, the evaluator's control
// flow types, and the value helpers the builtins share. Not a public header.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::tools::util::jq::detail {

// jq preserves object key order (insertion order), so the whole engine runs
// on ordered_json: `.scripts` prints in file order, `keys_unsorted` is real.
using json = nlohmann::ordered_json;

enum class K {
    Identity, Recurse, Index, Slice, Iterate, Literal, Str, Format,
    Pipe, Comma, Alt, And, Or, Neg, Bin, Array, Object, If, Try,
    Reduce, Foreach, As, Var, Call, Assign,
};

struct Node;
using NodeP = std::shared_ptr<const Node>;

struct Node {
    K           k{};
    std::string s;   // op (Bin/Assign), var name (Var/As/Reduce/Foreach), fn (Call), format (Format/Str)
    json        lit; // Literal value; for Str: array of bools, true = literal part
    NodeP       a, b, c, d;
    std::vector<NodeP> args;   // Call args | If conds | Object keys | Str parts
    std::vector<NodeP> args2;  // If branches | Object values (null = shorthand)
    std::size_t col = 0;       // 1-based column, for messages
};

struct SyntaxError { std::size_t col; std::string msg; };

// Parse a whole program. `col_base` offsets columns for interpolated
// sub-programs. Throws SyntaxError.
[[nodiscard]] NodeP parse(std::string_view src, std::size_t col_base = 0);

// ── evaluation control flow ────────────────────────────────────────────────
struct JqError    { json value; };                       // `error`, type errors: try can catch
struct Fatal      { std::string msg; };                  // limits: nothing catches it
struct Downstream { std::uint64_t id; json value; };     // an error raised AFTER a try's output
struct Stop       { std::uint64_t id; };                 // limit/first: abandon a generator

using Out     = std::function<void(const json&)>;
using PathOut = std::function<void(const json& path, const json& value)>;

struct Env {
    std::string name;
    json        value;
    std::shared_ptr<const Env> parent;
};
using EnvP = std::shared_ptr<const Env>;

struct Ctx {
    std::size_t   depth     = 0;
    std::uint64_t next_id   = 1;
    std::uint64_t steps     = 0;
    std::uint64_t max_steps = 5'000'000;
};

struct DepthGuard {
    Ctx& cx;
    explicit DepthGuard(Ctx& c) : cx(c) {
        if (++cx.depth > 512) throw Fatal{"recursion too deep (limit 512)"};
        if (++cx.steps > cx.max_steps)
            throw Fatal{"evaluation step limit exceeded (" + std::to_string(cx.max_steps) + ")"};
    }
    ~DepthGuard() { --cx.depth; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
};

// One unit of work for loops that emit without recursing (range, repeat,
// collecting into [...]): keeps `[range(1e9)]` inside the step budget
// instead of allocating until the process dies.
inline void tick(Ctx& cx) {
    if (++cx.steps > cx.max_steps)
        throw Fatal{"evaluation step limit exceeded (" + std::to_string(cx.max_steps) + ")"};
}

constexpr std::size_t kMaxCollection = 2'000'000;   // elements in one array/string build

void eval(Ctx&, const Node&, const json& in, const EnvP&, const Out&);
void eval_paths(Ctx&, const Node&, const json& in, const json& base, const EnvP&, const PathOut&);

// Builtins: return false when (name, arity) is unknown.
bool call_builtin(Ctx&, const Node& call, const json& in, const EnvP&, const Out&);
bool call_builtin_paths(Ctx&, const Node& call, const json& in, const json& base,
                        const EnvP&, const PathOut&);

// ── value helpers ──────────────────────────────────────────────────────────
[[noreturn]] void fail(std::string msg);
[[nodiscard]] bool        truthy(const json& v);
[[nodiscard]] int         cmp(const json& a, const json& b);   // jq total order
[[nodiscard]] bool        equal(const json& a, const json& b);
[[nodiscard]] json        num(double d);
[[nodiscard]] double      to_double(const json& v);
[[nodiscard]] std::string type_of(const json& v);
[[nodiscard]] std::string describe(const json& v);             // `number (1)`
[[nodiscard]] std::string tostring(const json& v);
[[nodiscard]] std::size_t cp_len(std::string_view s);
[[nodiscard]] std::size_t cp_to_byte(std::string_view s, std::size_t cp);
[[nodiscard]] std::size_t byte_to_cp(std::string_view s, std::size_t byte);
[[nodiscard]] json        index_value(const json& t, const json& idx);
[[nodiscard]] json        slice_value(const json& t, const json& lo, const json& hi);
[[nodiscard]] json        binop(const std::string& op, const json& l, const json& r);
[[nodiscard]] json        getpath(const json& v, const json& path);
void                      setpath(json& v, const json& path, std::size_t i, const json& val);
[[nodiscard]] json        delpaths(json v, json paths);
[[nodiscard]] std::string apply_format(const std::string& fmt, const json& v);

// Evaluate `n` and hand each output to `f` (sugar for arg closures).
inline void each(Ctx& cx, const Node& n, const json& in, const EnvP& env,
                 const std::function<void(const json&)>& f) {
    eval(cx, n, in, env, f);
}

// Run `body`, converting any error a CONSUMER downstream of `out` raises into
// a Downstream tagged with a fresh id, so the caller's error handling (try,
// `//`, `?`) only ever sees errors from its OWN body. Returns that id.
inline Out downstream_wrap(std::uint64_t id, const Out& out) {
    return [id, &out](const json& v) {
        try { out(v); }
        catch (JqError& e) { throw Downstream{id, std::move(e.value)}; }
    };
}

} // namespace mcp::tools::util::jq::detail
