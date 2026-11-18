// SPDX-License-Identifier: Apache-2.0
// The non-interactive, deterministic environment every child a tool runs
// gets: no colour, no pager, no credential prompt. Shared by shell,
// process_start and diagnostics/test so they behave alike.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace mcp::tools::util {

using EnvList = std::vector<std::pair<std::string, std::string>>;

// The defaults, then `extra` layered on top (later entries win).
[[nodiscard]] EnvList non_interactive_env(const EnvList& extra = {});

}  // namespace mcp::tools::util
