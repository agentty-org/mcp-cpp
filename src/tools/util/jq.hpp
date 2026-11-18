// jq.hpp — a self-contained implementation of the jq language for json_query.
//
// Covers what agents actually write: paths, slices, `..`, `?`, `//`, `|`/`,`,
// arithmetic and comparisons with jq's semantics and total order, `and`/`or`,
// `if`/`elif`/`else`, `try`/`catch`, `reduce`/`foreach`, `E as $x | …`,
// object/array construction, string interpolation, @formats (@csv @tsv @json
// @text @html @uri @sh @base64 @base64d), assignment (`=`, `|=`, `+=`, …,
// `//=`), and ~110 builtins (map, select, to_entries, group_by, sort_by,
// test/match/capture/sub/gsub, paths, del, walk, limit, …).
// Not supported: `def`, `label`/`break`, destructuring, dates, `input(s)`,
// `$ENV`/`env` (always {} so the host environment can't leak).
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::tools::util::jq {

// Run `program` against `input`. Returns every output, or an error message
// ("jq: syntax error at col 7: expected ']'", "jq: error: …"). More than
// `max_outputs` results, or exhausting the step/depth budget, is an error.
[[nodiscard]] std::expected<std::vector<nlohmann::ordered_json>, std::string>
run(std::string_view program, const nlohmann::ordered_json& input, std::size_t max_outputs = 10000);

} // namespace mcp::tools::util::jq
