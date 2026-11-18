// SPDX-License-Identifier: Apache-2.0
#include "child_env.hpp"

namespace mcp::tools::util {

EnvList non_interactive_env(const EnvList& extra) {
    EnvList env = {
        {"NO_COLOR", "1"},              // https://no-color.org
        {"CLICOLOR", "0"},
        {"CLICOLOR_FORCE", "0"},
        {"TERM", "dumb"},               // discourages cursor/colour escapes
        {"PAGER", "cat"},
        {"GIT_PAGER", "cat"},
        {"GIT_TERMINAL_PROMPT", "0"},   // never block on a credential prompt
        {"GIT_OPTIONAL_LOCKS", "0"},
        {"DEBIAN_FRONTEND", "noninteractive"},
        {"PYTHONUNBUFFERED", "1"},      // stream python output to the idle watchdog
    };
    for (const auto& kv : extra) env.push_back(kv);
    return env;
}

}  // namespace mcp::tools::util
