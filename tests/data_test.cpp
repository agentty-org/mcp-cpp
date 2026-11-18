// SPDX-License-Identifier: Apache-2.0
//
// data_test.cpp — json_query (jq) driven through make_provider(), plus the
// jq engine directly (cases cross-checked against jq 1.8.2).

#include "support/disk_files.hpp"
#include "../src/tools/util/jq.hpp"
#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/toolset.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/local.hpp>

#include "agtest.hpp"
#include "test_state.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#  include <process.h>
#  define mcp_getpid _getpid
#else
#  include <unistd.h>
#  define mcp_getpid getpid
#endif

using namespace mcp::tools;
namespace fs = std::filesystem;

static mcp::cap::Result call(mcp::cap::CapabilityProvider& p,
                             const std::string& name, mcp::Json args) {
    return p.execute(mcp::cap::Request{name, std::move(args)});
}
static mcp::Json obj() { return mcp::Json::object(); }
static bool has(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}
static void wr(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), (std::streamsize)s.size());
}

TEST_CASE("json_query") {
    auto root = fs::temp_directory_path() / ("mcp_data_test_" + std::to_string(mcp_getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    auto ws_state = mcp::test::state_at(root);
    auto prev = fs::current_path();
    fs::current_path(root);

    HostServices svc;

    svc.files = std::make_shared<mcp::test::DiskFiles>();
    svc.state = ws_state;
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    const std::string pkg = R"({
      "name": "demo",
      "version": "1.2.3",
      "scripts": { "build": "tsc", "test": "vitest", "lint": "eslint ." },
      "dependencies": { "react": "^18.0.0", "zod": "^3.0.0" },
      "services": [
        { "name": "api",  "port": 8080 },
        { "name": "web",  "port": 3000 },
        { "name": "cron", "port": 0 }
      ]
    })";
    wr(root / "package.json", pkg);
    auto pj = obj(); pj["path"] = (root / "package.json").string();

    auto q = [&](const std::string& query, bool raw=false) {
        auto a = pj; a["query"] = query; if (raw) a["raw"] = true;
        return call(*provider, "json_query", a);
    };

    // ── scalar member ────────────────────────────────────────────────────
    {
        auto r = q(".version");
        check(!r.is_error, ".version runs");
        check(has(r.text, "1.2.3"), ".version returns the value");
        check(r.effects.has(Effect::ReadFs), "json_query is ReadFs");
    }
    // raw string
    {
        auto r = q(".name", /*raw=*/true);
        check(r.text == "demo", "raw:true unquotes a string result");
    }
    // ── nested member ────────────────────────────────────────────────────
    {
        auto r = q(".dependencies.react", true);
        check(r.text == "^18.0.0", "nested .a.b path");
    }
    // ── keys pipeline ────────────────────────────────────────────────────
    {
        auto r = q(".scripts | keys");
        check(!r.is_error, "| keys runs");
        check(has(r.text, "build") && has(r.text, "test") && has(r.text, "lint"),
              "| keys lists the object's keys");
    }
    // ── length ───────────────────────────────────────────────────────────
    {
        auto r = q(".services | length");
        check(r.text == "3", "| length of an array");
    }
    // ── array index (incl negative) ──────────────────────────────────────
    {
        auto r = q(".services[0].name", true);
        check(r.text == "api", "positive index + member");
        auto r2 = q(".services[-1].name", true);
        check(r2.text == "cron", "negative index counts from the end");
    }
    // ── iterate: one result line per element ─────────────────────────────
    {
        auto r = q(".services[].name", true);
        check(!r.is_error, "[] iterate runs");
        check(has(r.text, "api") && has(r.text, "web") && has(r.text, "cron"),
              "iterate yields every element's field");
        // three lines
        check(std::count(r.text.begin(), r.text.end(), '\n') == 2,
              "one result per iterated element (3 lines)");
    }
    // ── slice ────────────────────────────────────────────────────────────
    {
        auto r = q(".services[0:2] | length");
        check(r.text == "2", "slice [0:2] then length");
    }
    // ── type / has ───────────────────────────────────────────────────────
    {
        check(q(".services | type").text == "\"array\"", "| type reports array");
        check(q(".version | type").text == "\"string\"", "| type reports string");
        check(q("has(\"scripts\")").text == "true", "has(k) true for present key");
        check(q("has(\"nope\")").text == "false", "has(k) false for absent key");
    }
    // ── identity ─────────────────────────────────────────────────────────
    {
        auto r = q(".");
        check(!r.is_error && has(r.text, "\"name\""), ". returns the whole doc");
    }
    // ── inline json (no file) ────────────────────────────────────────────
    {
        auto a = obj();
        a["json"] = R"({"a":{"b":[10,20,30]}})";
        a["query"] = ".a.b[1]";
        auto r = call(*provider, "json_query", a);
        check(!r.is_error && r.text == "20", "inline json + index path");
    }
    // ── error paths ──────────────────────────────────────────────────────
    {
        // bad JSON
        wr(root / "bad.json", "{ not json ");
        auto a = obj(); a["path"] = (root / "bad.json").string(); a["query"] = ".";
        check(call(*provider, "json_query", a).is_error, "invalid JSON is an error");
        // member on non-object
        check(q(".version.nope").is_error, ".key on a scalar is an error");
        // neither path nor json
        auto b = obj(); b["query"] = ".";
        check(call(*provider, "json_query", b).is_error, "missing path AND json is an error");
        // unknown verb
        check(q(".scripts | frobnicate").is_error, "unknown pipeline verb is an error");
        // a syntax error names the column
        auto se = q(".services[");
        check(se.is_error && has(se.text, "col"), "syntax error carries a column");
    }

    // ── the jq engine (util::jq) — real jq semantics ─────────────────────
    {
        namespace jq = mcp::tools::util::jq;
        using oj = nlohmann::ordered_json;
        const oj doc = oj::parse(R"({
          "deps": {"react": "^18.2.0", "zod": "3.22.4", "lodash": "^4.17.21"},
          "svc": [
            {"name": "api", "port": 8080, "kind": "web", "ok": true, "n": 3},
            {"name": "db", "port": 5432, "kind": "data", "ok": false, "n": 1},
            {"name": "cache", "kind": "data", "ok": true, "n": 5}
          ],
          "nested": {"a": {"port": 1, "b": [{"port": 2}, {"x": null}]}},
          "nums": [3, 1.5, -2, 10],
          "text": "héllo wörld 42"
        })");
        // Each case: query -> expected newline-joined compact outputs.
        const std::pair<const char*, const char*> cases[] = {
            {".deps | to_entries | map(select(.value | startswith(\"^\"))) | map(.key)", R"(["react","lodash"])"},
            {"[.. | .port? // empty]", "[8080,5432,1,2]"},
            {".svc | group_by(.kind) | map({kind: .[0].kind, n: length})", R"([{"kind":"data","n":2},{"kind":"web","n":1}])"},
            {"[.svc[] | select(.n > 2 and .ok) | .name]", R"(["api","cache"])"},
            {".missing // \"d\"", R"("d")"},
            {"[.svc[].port // 0]", "[8080,5432]"},        // `//` applies to the whole stream
            {"[.svc[] | .port // 0]", "[8080,5432,0]"},
            {".nums[1:3]", "[1.5,-2]"},
            {".nums[-1]", "10"},
            {".nums | add", "12.5"},
            {".nums | sort", "[-2,1.5,3,10]"},
            {".nums | min, max", "-2\n10"},
            {".svc | sort_by(.n) | map(.name)", R"(["db","api","cache"])"},
            {".svc | max_by(.n) | .name", R"("cache")"},
            {".svc | unique_by(.kind) | length", "2"},
            {".deps | keys", R"(["lodash","react","zod"])"},
            {".deps | keys_unsorted", R"(["react","zod","lodash"])"},
            {"{name: .svc[0].name, count: (.svc | length)}", R"({"name":"api","count":3})"},
            {"\"\\(.svc[0].name):\\(.svc[0].port)\"", R"("api:8080")"},
            {"reduce .svc[] as $s ({}; .[$s.kind] += [$s.name])", R"({"web":["api"],"data":["db","cache"]})"},
            {"[foreach .nums[] as $n (0; . + $n)]", "[3,4.5,2.5,12.5]"},
            {"[limit(2; .svc[].name)]", R"(["api","db"])"},
            {"first(.svc[] | select(.kind == \"data\")) | .name", R"("db")"},
            {"[range(1; 10; 3)]", "[1,4,7]"},
            {"if (.svc | length) > 2 then \"many\" else \"few\" end", R"("many")"},
            {"try error(\"boom\") catch .", R"("boom")"},
            {"[.svc[] | try (if .ok then .name else error(\"x\") end) catch \"caught\"]", R"(["api","caught","cache"])"},
            {".text | test(\"WÖRLD\"; \"i\")", "true"},
            {".text | [match(\"\\\\w+\"; \"g\") | .string]", R"(["héllo","wörld","42"])"},
            {".text | capture(\"(?<n>[0-9]+)\") | .n", R"("42")"},
            {".text | gsub(\"[aeiou]\"; \"_\")", R"("héll_ wörld 42")"},
            {".text | length", "14"},
            {".text | split(\" \")", R"(["héllo","wörld","42"])"},
            {"[.svc[].name] | join(\",\")", R"("api,db,cache")"},
            {"[.svc[].name] | @csv", R"("\"api\",\"db\",\"cache\"")"},
            {"del(.svc[] | select(.ok | not)) | .svc | map(.name)", R"(["api","cache"])"},
            {".svc[].n |= . * 10 | [.svc[].n]", "[30,10,50]"},
            {".svc[0].port = 1 | .svc[0].port", "1"},
            {".newkey //= \"x\" | .newkey", R"("x")"},
            {"[paths(type == \"number\")] | length", "11"},
            {".nested | getpath([\"a\", \"b\", 0, \"port\"])", "2"},
            {"pick(.svc[1].name) | .svc[1]", R"({"name":"db"})"},
            {"walk(if type == \"number\" then . + 1 else . end) | .nums", "[4,2.5,-1,11]"},
            {".svc | INDEX(.name) | keys", R"(["api","cache","db"])"},
            {"IN(.svc[].name; \"db\")", "true"},
            {".svc | any(.port == 5432), all(.ok)", "true\nfalse"},
            {"(.nums | length) as $n | .nums | add / $n", "3.125"},
            {"[1, [2, [3]]] | flatten", "[1,2,3]"},
            {"{\"a\": {\"x\": 1}} * {\"a\": {\"y\": 2}}", R"({"a":{"x":1,"y":2}})"},
            {"[1,2,3,1] - [1]", "[2,3]"},
            {".svc | map(has(\"port\"))", "[true,true,false]"},
            {".missing.deeper", "null"},
            {"[.[]?]  | length", "5"},
            {".svc[0] | with_entries(select(.value | type == \"number\"))", R"({"port":8080,"n":3})"},
            {"[.svc[] | .name | ascii_upcase]", R"(["API","DB","CACHE"])"},
            {"\"10\" | tonumber + 1", "11"},
            {"[1, null, 2] | map(values)", "[1,2]"},
            {"$__loc__ | .line", "1"},
            // old jq-lite spellings keep working
            {"[0].x", "null"},
        };
        int fails = 0;
        for (const auto& [query, want] : cases) {
            auto r = jq::run(query, query == std::string_view{"[0].x"} ? oj::array({oj::object()}) : doc);
            std::string got;
            if (!r) got = "ERR: " + r.error();
            else for (std::size_t i = 0; i < r->size(); ++i) got += (i ? "\n" : "") + (*r)[i].dump();
            if (got != want) {
                ++fails;
                std::fprintf(stderr, "jq case failed: %s\n  want: %s\n  got:  %s\n", query, want, got.c_str());
            }
        }
        check(fails == 0, "every jq case matches real jq output");

        // Errors are reported, with the column for syntax errors.
        auto e1 = jq::run(".a[", doc);
        check(!e1 && has(e1.error(), "syntax error at col"), "syntax error has a column");
        auto e2 = jq::run(".nums | join(\",\")", doc);
        check(e2.has_value(), "join stringifies numbers");
        auto e3 = jq::run(".text | .foo", doc);
        check(!e3 && has(e3.error(), "Cannot index string"), "type errors are jq's wording");
        auto e4 = jq::run("nosuch(1)", doc);
        check(!e4 && has(e4.error(), "nosuch/1"), "unknown function names its arity");
        // Resource guards: none of these may hang or exhaust memory.
        check(!jq::run("[range(1e9)] | length", doc), "huge array is capped");
        check(!jq::run("reduce range(1e9) as $i (0; . + 1)", doc), "step budget stops runaway loops");
        check(!jq::run("\"x\" * 1e9", doc), "string blow-up is capped");
        check(!jq::run("\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!\" | test(\"(a+)+$\")", doc), "ReDoS shape refused");
        auto many = jq::run("range(50)", doc, 10);
        check(!many && has(many.error(), "more than 10"), "output cap is enforced");
        check(jq::run("env | length", doc).value()[0] == 0, "env never leaks the host environment");
    }

    // ── tool-level: null hint, no-output note, BOM ───────────────────────────
    {
        auto r = q(".dependncies.react");   // typo
        check(!r.is_error && has(r.text, "missing key yields null"), "all-null results explain why");
        auto e = q(".services[] | select(.port > 99999)");
        check(!e.is_error && has(e.text, "no output"), "empty result says so");
        wr(root / "bom.json", "\xEF\xBB\xBF{\"k\": 1}");
        auto a = obj(); a["path"] = (root / "bom.json").string(); a["query"] = ".k";
        auto b = call(*provider, "json_query", a);
        check(!b.is_error && b.text == "1", "a UTF-8 BOM is skipped");
    }

    fs::current_path(prev);
    fs::remove_all(root);
}
