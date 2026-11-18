// SPDX-License-Identifier: Apache-2.0
//
// data.cpp — register_data_tools: json_query, jq over structured data.
//
// grep/extract are line-oriented and blind to structure; a config or data
// file wants a PATH query, not a regex. json_query runs a jq program (the
// real language, see util/jq.hpp: paths, select/map, `..`, `//`, `?`,
// reduce, regex, @formats, ~110 builtins) over a JSON document (a `.json`
// file, or inline `json`). Key order is preserved, as in jq. JSON only;
// YAML/TOML would need a new parser dependency.

#include "tool_body.hpp"
#include "tool_shell.hpp"
#include "call_state.hpp"
#include "util/jq.hpp"

#include <mcp/tools/util/error.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/arg_reader.hpp>
#include <mcp/tools/util/utf8.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::tools::detail {
namespace {

namespace fs = std::filesystem;
using util::ToolError;
using util::ExecResult;
using util::ToolOutput;
using ojson = nlohmann::ordered_json;

constexpr std::size_t kMaxOutputBytes = 20'000;
constexpr std::size_t kMaxResults     = 2000;    // shown
constexpr std::size_t kMaxEvaluated   = 100'000; // produced before we stop

struct QueryArgs {
    std::string path;      // file path (mutually exclusive-ish with json_text)
    std::string json_text; // inline JSON
    std::string query = ".";
    bool        raw = false;   // print strings unquoted
    bool        compact = true;
    std::string display_description;
};

const util::ArgSpec<QueryArgs>& kQueryArgs() {
    static const util::ArgSpec<QueryArgs> spec{
        util::arg(&QueryArgs::display_description, "display_description",
                  "One-line summary shown in the UI. Optional."),
        util::arg(&QueryArgs::query, "query",
                  "A jq program (default `.`). E.g. `.dependencies.react`, `.services[].port`, "
                  "`.scripts | keys`, `.items | map(select(.ok and .n > 2)) | length`, "
                  "`[.. | .port? // empty]`, `.deps | to_entries | map(select(.value | startswith(\"^\"))) | map(.key)`, "
                  "`group_by(.kind) | map({kind: .[0].kind, n: length})`, `\"\\(.name)@\\(.version)\"`."),
        util::arg(&QueryArgs::path, "path", "A .json file to query. Provide this OR inline `json`."),
        util::arg(&QueryArgs::json_text, "json", "Inline JSON to query instead of a file."),
        util::arg(&QueryArgs::raw, "raw", "Print string results unquoted (jq -r)."),
        util::arg(&QueryArgs::compact, "compact",
                  "Compact one-line JSON per result (default true); false pretty-prints."),
    };
    return spec;
}

// An empty query means the whole doc; a file or inline JSON is needed.
std::expected<QueryArgs, ToolError> check_query(QueryArgs a, const util::Bounds&) {
    if (a.query.empty()) a.query = ".";
    if (a.path.empty() && a.json_text.empty())
        return std::unexpected(ToolError::invalid_args(
            "provide `path` (a .json file) or inline `json`"));
    return a;
}

// The most common dead end: a typo'd key yields `null` (jq semantics), and
// the model reads that as "the value is null". Name the likely cause.
std::string null_hint(const std::string& query, const std::vector<ojson>& outs) {
    if (outs.empty() || !std::ranges::all_of(outs, [](const ojson& v) { return v.is_null(); }))
        return {};
    if (query.find('.') == std::string::npos) return {};
    return "\n(every result is null: in jq a missing key yields null. Check the "
           "spelling with `keys`, or `has(\"k\")`.)";
}

ExecResult run_json_query(const Call& call, const QueryArgs& a) {
    std::string text;
    if (!a.path.empty()) {
        auto wp = util::make_readable_path_checked(a.path, "json_query", bounds(call));
        if (!wp) return std::unexpected(std::move(wp.error()));
        const auto& p = wp->path();
        const auto st = call.fs().stat(p);
        if (!st.exists)  return std::unexpected(ToolError::not_found(wp->string()));
        if (!st.regular) return std::unexpected(ToolError::not_a_file(wp->string()));
        if (st.size > 16u*1024u*1024u)
            return std::unexpected(ToolError::too_large("file > 16 MiB"));
        try { text = util::read_file(*wp); }
        catch (...) { return std::unexpected(ToolError::io("cannot read " + wp->string())); }
    } else {
        text = a.json_text;
    }
    // A BOM is common in Windows-saved JSON and is not JSON; skip it.
    const bool had_bom = text.starts_with("\xEF\xBB\xBF");
    std::string_view body_text = text;
    if (had_bom) body_text.remove_prefix(3);

    ojson doc;
    try { doc = ojson::parse(body_text); }
    catch (const ojson::parse_error& e) {
        // nlohmann's what() is "[json.exception.parse_error.101] parse error at
        // line 1, column 4: syntax error ..."; keep the useful middle.
        std::string why = e.what();
        if (auto p = why.find("] "); p != std::string::npos) why.erase(0, p + 2);
        return std::unexpected(ToolError::invalid_args("not valid JSON: " + why));
    }

    auto outs = util::jq::run(a.query, doc, kMaxEvaluated);
    if (!outs)
        return std::unexpected(ToolError::invalid_args(
            "query `" + a.query + "` failed — " + outs.error()));

    if (outs->empty())
        return ToolOutput{"(no output \u2014 the query produced no results, e.g. `select` "
                          "filtered everything or `.[]` iterated an empty container)",
                          std::nullopt};

    std::ostringstream out;
    std::size_t shown = 0;
    for (const auto& v : *outs) {
        if (shown++ >= kMaxResults) {
            out << "\u2026 " << (outs->size() - kMaxResults) << " more results "
                   "(narrow the query, or wrap it in `[ ... ] | length`)\n";
            break;
        }
        if (a.raw && v.is_string()) out << v.get<std::string>() << "\n";
        else out << (a.compact ? v.dump(-1, ' ', false, ojson::error_handler_t::replace)
                               : v.dump(2, ' ', false, ojson::error_handler_t::replace)) << "\n";
        if (static_cast<std::size_t>(out.tellp()) >= kMaxOutputBytes) {
            out << "[capped at " << kMaxOutputBytes << " bytes after " << shown << " of "
                << outs->size() << " results]\n";
            break;
        }
    }
    std::string body = out.str();
    if (!body.empty() && body.back() == '\n') body.pop_back();
    body += null_hint(a.query, *outs);
    return ToolOutput{util::to_valid_utf8(std::move(body)), std::nullopt};
}

} // namespace

void register_data_tools(Shells& sh) {
    sh.add("json_query",
        "USE WHEN you need a value OUT of a .json file (package.json, tsconfig, "
        "compile_commands.json, an API fixture, an MCP config) — query it with jq "
        "instead of `read`-ing the whole file and eyeballing it, or grep-ing blind "
        "to structure. Point it at a `.json` file (or inline `json`) and a jq "
        "program. Real jq: `.a.b`, `.[i]`/`.[lo:hi]` (negative from end), `.[]`, "
        "`..`, `?`, `//`, `|`, `,`, arithmetic/comparison, `and`/`or`/`not`, "
        "`if`/`elif`/`else`, `try`/`catch`, `reduce`/`foreach`, `E as $x | …`, "
        "`{…}`/`[…]` construction, string interpolation, @csv/@tsv/@json/@sh/@uri/"
        "@base64, assignment (`=`, `|=`, `+=`, `//=`), and builtins incl. map, "
        "select, keys, has, length, to_entries/from_entries/with_entries, sort_by, "
        "group_by, unique_by, min_by/max_by, add, any/all, flatten, paths, del, "
        "pick, walk, limit/first/last, range, split/join, test/match/capture/sub/"
        "gsub, ascii_downcase, tostring/tonumber, INDEX/IN. Not supported: `def`, "
        "`label`, destructuring, dates, `$ENV`. Each result prints as one compact "
        "JSON line (`raw:true` unquotes strings). JSON only.",
        kQueryArgs().schema(), EffectSet{Effect::ReadFs},
        body<QueryArgs>(run_json_query, kQueryArgs(), check_query), 25'000);
}

} // namespace mcp::tools::detail
