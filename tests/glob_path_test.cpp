// glob_path_test — the one glob every walking tool uses, and the skip-dir
// escape hatch that lets a glob reach into third_party/ or vendor/.
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/tools/util/glob.hpp>

#include <cstdio>
#include <string>

namespace {
int fails = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::printf("FAIL %s\n", what.c_str()); ++fails; }
}
bool m(const char* pat, const char* rel) { return mcp::tools::util::glob_path_match(pat, rel); }
}  // namespace

int main() {
    using mcp::tools::util::glob_names_dir;
    using mcp::tools::util::should_skip_dir;
    using mcp::tools::util::skip_dir_rg_globs;

    // Basename patterns match the file name at any depth.
    check(m("*.cpp", "a.cpp") && m("*.cpp", "src/x/a.cpp"), "*.cpp anywhere");
    check(!m("*.cpp", "src/a.hpp"), "*.cpp not .hpp");
    // Braces, nested.
    check(m("*.{ts,tsx}", "web/a.tsx") && m("*.{ts,tsx}", "a.ts") && !m("*.{ts,tsx}", "a.js"), "braces");
    check(m("src/{api,web}/**/*.go", "src/web/x/y/a.go") && !m("src/{api,web}/**/*.go", "src/cli/a.go"),
          "braces in a path");
    check(m("*.{c,{cc,cpp}}", "a.cc"), "nested braces");
    // Path patterns: segment-wise, ** spans zero or more segments.
    check(m("src/*.cpp", "src/a.cpp") && !m("src/*.cpp", "src/x/a.cpp"), "* stays in one segment");
    check(m("src/**/*.cpp", "src/a.cpp") && m("src/**/*.cpp", "src/x/y/a.cpp"), "** spans 0..n");
    check(m("**/util/*.hpp", "util/a.hpp") && m("**/util/*.hpp", "x/y/util/a.hpp"), "leading **");
    check(m("src/**", "src/a/b/c.txt") && !m("src/**", "lib/a.txt"), "trailing **");
    check(m("./src/*.cpp", "src/a.cpp") && m("/src/*.cpp", "src/a.cpp"), "./ and / prefixes");
    check(m("third_party/mcp-cpp/src/tools/util/jq_builtins.cpp",
            "third_party/mcp-cpp/src/tools/util/jq_builtins.cpp"), "literal path");
    check(m("", "anything"), "empty matches all");

    // A glob that names a skip-listed dir lets the walker into it.
    check(glob_names_dir("third_party/mcp-cpp/src/*.cpp", "third_party"), "names third_party");
    check(glob_names_dir("**/vendor/*.go", "vendor"), "names vendor under **");
    check(glob_names_dir("{third_party,src}/**/*.c", "third_party"), "names via braces");
    check(!glob_names_dir("*.cpp", "third_party"), "basename glob names no dir");
    check(!glob_names_dir("third_*/x", "third_party"), "a wildcard segment names nothing");
    check(should_skip_dir("node_modules", "") && !should_skip_dir("node_modules", "node_modules/x/*.js"),
          "should_skip_dir honours the glob");
    check(should_skip_dir("build", "src/**/*.cpp"), "unrelated glob keeps the skip");

    // ripgrep excludes drop exactly the ones the glob reaches into.
    const auto has = [](const std::vector<std::string>& v, const std::string& g) {
        for (std::size_t i = 1; i < v.size(); i += 2) if (v[i] == g) return true;
        return false;
    };
    const auto all = skip_dir_rg_globs("");
    check(has(all, "!third_party") && has(all, "!build*"), "default excludes present");
    const auto tp = skip_dir_rg_globs("third_party/maya/**/*.hpp");
    check(!has(tp, "!third_party") && has(tp, "!node_modules"), "third_party exclude dropped, rest kept");
    const auto bd = skip_dir_rg_globs("build-rel/compile_commands.json");
    check(!has(bd, "!build*") && has(bd, "!third_party"), "build* family dropped for build-rel/");
    check(tp.size() % 2 == 0, "argv pairs stay paired");

    if (fails == 0) std::printf("glob_path_test: all passed\n");
    return fails == 0 ? 0 : 1;
}
