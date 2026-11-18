// jq.cpp — the public entry point: parse, evaluate, collect, cap.
#include "jq.hpp"
#include "jq_ast.hpp"

#include <regex>

namespace mcp::tools::util::jq {

namespace {

// json_query's old "jq-lite" accepted a leading bracket as a PATH (`[0].id`,
// `[].name`, `["k"]`); in real jq that's array construction. Keep the old
// spelling working by reading a leading `[]`, `[n]`, `[a:b]` or `["k"]`
// followed by a path continuation as `.[…]`.
std::string compat(std::string_view p) {
    std::size_t i = 0;
    while (i < p.size() && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n')) ++i;
    std::string s{p.substr(i)};
    if (s.starts_with("|")) s.erase(0, 1);   // old: a leading `| keys`
    static const std::regex leading(R"(^\[\s*(|-?\d+|-?\d*\s*:\s*-?\d*|"[^"]*")\s*\]\s*($|[.\[|]))");
    if (std::regex_search(s, leading)) s.insert(0, ".");
    return s;
}

} // namespace

std::expected<std::vector<nlohmann::ordered_json>, std::string>
run(std::string_view program, const nlohmann::ordered_json& input, std::size_t max_outputs) {
    using namespace detail;
    const std::string src = compat(program);
    NodeP ast;
    try {
        ast = parse(src.empty() ? "." : src);
    } catch (const SyntaxError& e) {
        return std::unexpected("jq: syntax error at col " + std::to_string(e.col) + ": " + e.msg);
    }
    std::vector<json> outs;
    Ctx cx;
    try {
        eval(cx, *ast, input, nullptr, [&](const json& v) {
            if (outs.size() >= max_outputs)
                throw Fatal{"more than " + std::to_string(max_outputs) + " results; narrow the query"};
            outs.push_back(v);
        });
    } catch (const JqError& e) {
        const std::string msg = e.value.is_string() ? e.value.get<std::string>()
                              : e.value.dump(-1, ' ', false, json::error_handler_t::replace) + " (not a string)";
        return std::unexpected("jq: error: " + msg);
    } catch (const Downstream& d) {   // unreachable in practice; keep it safe
        return std::unexpected("jq: error: " + tostring(d.value));
    } catch (const Stop&) {
        // a stray Stop means a builtin forgot its catch; treat as end of output
    } catch (const Fatal& f) {
        return std::unexpected("jq: " + f.msg);
    }
    return outs;
}

} // namespace mcp::tools::util::jq
