// SPDX-License-Identifier: Apache-2.0
#pragma once
// fnmatch-style glob matcher. Supports `*` (any run, incl. empty),
// `?` (any single char), `[abc]` / `[a-z]` / `[!abc]` character classes.
// Case-insensitive on Windows, case-sensitive elsewhere. `**` collapses to
// `*` (no path-spanning); callers match against just the filename segment.

#include <filesystem>
#include <string_view>

namespace mcp::tools::util {

[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view name) noexcept;

// THE file-filter glob every walking tool uses (grep, glob, replace, extract,
// aggregate, search_structural, rewrite_structural), so a `glob` argument
// means one thing everywhere:
//   - `{a,b}` alternation, nestable: `*.{ts,tsx}`, `src/{api,web}/**/*.go`.
//   - no '/' in the pattern: matched against the FILENAME (`*.cpp`).
//   - a '/' in it: matched against the path relative to `root`, segment by
//     segment; `*` `?` `[..]` stay inside one segment and `**` spans zero or
//     more whole segments (`src/**/*.ts` hits src/a.ts and src/x/y/a.ts).
//     A leading `./` or `/` is ignored.
// Empty pattern matches everything.
[[nodiscard]] bool glob_path_match(std::string_view pattern,
                                   const std::filesystem::path& file,
                                   const std::filesystem::path& root);

// Same, on an already-relative generic ('/'-separated) path.
[[nodiscard]] bool glob_path_match(std::string_view pattern, std::string_view rel_path);

} // namespace mcp::tools::util
