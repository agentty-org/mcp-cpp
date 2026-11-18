// SPDX-License-Identifier: Apache-2.0
//
// edit_stress — the edit tool, driven through make_provider() exactly as an
// agent drives it, against the shapes that break editors in practice:
//
//   ROBUSTNESS  every case asserts the bytes on disk afterwards, not just the
//               message: CRLF / mixed endings, BOM, no trailing newline,
//               unicode, tabs, symlinks, file mode, regex specials in
//               old_text, $-patterns in new_text, overlapping and
//               order-dependent edit batches, ambiguous matches, a randomized
//               differential against a reference string-replace.
//   SPEED       timed on 1 MiB / 30k-line files, including the MISS path
//               (no exact hit, so the fuzzy matcher runs over the whole file),
//               which is where an O(n*m) matcher hides.
//
// Exit 0 when every check passes. `--bench` prints timings only.

#include "test_state.hpp"
#include "test_exec.hpp"
#include "support/disk_files.hpp"
#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/cap/local.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace mcp::tools;
using Json = mcp::Json;

namespace {

int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

fs::path g_root;
std::shared_ptr<mcp::cap::CapabilityProvider> g_p;

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
void spit(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s;
}

struct R { bool err; std::string text; double ms; };
R call(const std::string& tool, Json args) {
    mcp::cap::Request req{tool, std::move(args)};
    const auto t0 = std::chrono::steady_clock::now();
    auto r = g_p->execute(req);
    const auto t1 = std::chrono::steady_clock::now();
    return {r.is_error, r.text, std::chrono::duration<double, std::milli>(t1 - t0).count()};
}
R edit1(const fs::path& f, const std::string& o, const std::string& n, Json extra = Json::object()) {
    Json e{{"old_text", o}, {"new_text", n}};
    for (auto& [k, v] : extra.items()) e[k] = v;
    return call("edit", {{"path", f.string()}, {"edits", Json::array({e})}});
}
R edits(const fs::path& f, Json arr) { return call("edit", {{"path", f.string()}, {"edits", arr}}); }

std::string big_source(int lines, unsigned seed) {
    std::mt19937 rng(seed);
    std::string s;
    s.reserve(static_cast<std::size_t>(lines) * 36);
    for (int i = 0; i < lines; ++i) {
        const int ind = static_cast<int>(rng() % 4) * 4;
        s.append(static_cast<std::size_t>(ind), ' ');
        s += "auto v" + std::to_string(i) + " = compute(" + std::to_string(rng() % 1000) + ");\n";
    }
    return s;
}

// ── robustness ───────────────────────────────────────────────────────────
void robustness() {
    const auto f = g_root / "r.txt";

    // Exact edit; untouched bytes stay untouched.
    spit(f, "alpha\nbeta\ngamma\n");
    CHECK(!edit1(f, "beta", "BETA").err);
    CHECK(slurp(f) == "alpha\nBETA\ngamma\n");

    // No trailing newline is preserved (no newline invented at EOF).
    spit(f, "a\nb\nc");
    CHECK(!edit1(f, "b", "B").err);
    CHECK(slurp(f) == "a\nB\nc");
    CHECK(!edit1(f, "c", "C").err);
    CHECK(slurp(f) == "a\nB\nC");

    // CRLF file keeps CRLF, model text written with \n.
    spit(f, "one\r\ntwo\r\nthree\r\n");
    CHECK(!edit1(f, "two\nthree", "2\n3").err);
    CHECK(slurp(f) == "one\r\n2\r\n3\r\n");

    // BOM preserved and not part of matching.
    spit(f, "\xEF\xBB\xBFhead\nbody\n");
    CHECK(!edit1(f, "head", "HEAD").err);
    CHECK(slurp(f) == "\xEF\xBB\xBFHEAD\nbody\n");

    // Unicode, including a multibyte char at EOF with no newline.
    spit(f, "na\xC3\xAFve caf\xC3\xA9");
    CHECK(!edit1(f, "caf\xC3\xA9", "\xE2\x98\x95").err);
    CHECK(slurp(f) == "na\xC3\xAFve \xE2\x98\x95");

    // Regex metacharacters in old_text are literal; $1 / $& in new_text too.
    spit(f, "x = a[i] + (b*c) ^ $d;\n");
    CHECK(!edit1(f, "a[i] + (b*c) ^ $d", "$1 $& \\n").err);
    CHECK(slurp(f) == "x = $1 $& \\n;\n");

    // Tabs: exact tab match keeps tabs.
    spit(f, "fn() {\n\treturn 1;\n}\n");
    CHECK(!edit1(f, "\treturn 1;", "\treturn 2;").err);
    CHECK(slurp(f) == "fn() {\n\treturn 2;\n}\n");

    // Ambiguous: refused, file untouched.
    spit(f, "dup\ndup\n");
    auto amb = edit1(f, "dup", "X");
    CHECK(amb.err);
    CHECK(slurp(f) == "dup\ndup\n");
    // ...replace_all does both; expected_replacements guards the count.
    CHECK(!edit1(f, "dup", "X", {{"replace_all", true}}).err);
    CHECK(slurp(f) == "X\nX\n");
    spit(f, "dup\ndup\n");
    CHECK(edit1(f, "dup", "X", {{"expected_replacements", 3}}).err);
    CHECK(slurp(f) == "dup\ndup\n");

    // Not found: error, untouched.
    spit(f, "keep me\n");
    CHECK(edit1(f, "absent text entirely", "x").err);
    CHECK(slurp(f) == "keep me\n");

    // Empty old_text never matches "everywhere".
    CHECK(edit1(f, "", "x").err);
    CHECK(slurp(f) == "keep me\n");

    // Deleting text (new_text empty).
    spit(f, "a\nREMOVE\nb\n");
    CHECK(!edit1(f, "REMOVE\n", "").err);
    CHECK(slurp(f) == "a\nb\n");

    // Batch: edits apply in order against the evolving buffer.
    spit(f, "one two three\n");
    CHECK(!edits(f, Json::array({{{"old_text", "one"}, {"new_text", "1"}},
                                 {{"old_text", "1 two"}, {"new_text", "12"}}})).err);
    CHECK(slurp(f) == "12 three\n");

    // Indentation drift: the fuzzy path re-indents, the rest is untouched.
    spit(f, "class A {\n    void f() {\n        g();\n    }\n};\n");
    // A fuzzy match on a file changed behind the tool's back is refused (the
    // stale guard) -- so read it first, as an agent would.
    {
        auto st = edit1(f, "void f() {\n    g();\n}", "void f() {\n    h();\n}");
        CHECK(st.err && st.text.find("changed on disk") != std::string::npos);
        CHECK(slurp(f) == "class A {\n    void f() {\n        g();\n    }\n};\n");
    }
    (void)call("read", {{"path", f.string()}});
    {
        auto r = edit1(f, "void f() {\n    g();\n}", "void f() {\n    h();\n}");
        CHECK(!r.err);
    }
    CHECK(slurp(f) == "class A {\n    void f() {\n        h();\n    }\n};\n");

    // old_text pasted from a `read` (line-number prefixes) still applies.
    spit(f, "first\nsecond\n");
    CHECK(!edit1(f, "     2\tsecond", "     2\tSECOND").err);
    CHECK(slurp(f) == "first\nSECOND\n");

    // File mode survives the atomic rename.
    spit(f, "#!/bin/sh\necho hi\n");
    ::chmod(f.c_str(), 0755);
    CHECK(!edit1(f, "echo hi", "echo bye").err);
    struct ::stat st{};
    ::stat(f.c_str(), &st);
    CHECK((st.st_mode & 0777) == 0755);

    // Editing through a symlink edits the target and keeps the link.
    const auto tgt = g_root / "target.txt";
    const auto lnk = g_root / "link.txt";
    spit(tgt, "via link\n");
    fs::remove(lnk);
    fs::create_symlink(tgt.filename(), lnk);
    CHECK(!edit1(lnk, "via link", "edited").err);
    CHECK(fs::is_symlink(lnk));
    CHECK(slurp(tgt) == "edited\n");

    // No temp files left behind.
    int stray = 0;
    for (auto& e : fs::directory_iterator(g_root))
        if (e.path().filename().string().find(".agentty-tmp") != std::string::npos
            || e.path().filename().string().find(".tmp") != std::string::npos) ++stray;
    CHECK(stray == 0);

    // Binary file refused, untouched.
    const auto bin = g_root / "b.bin";
    spit(bin, std::string("ab\0cd", 5));
    CHECK(edit1(bin, "ab", "xy").err);
    CHECK(slurp(bin) == std::string("ab\0cd", 5));

    // A read-only file stays read-only: the atomic rename would otherwise
    // replace it straight through the permission bit (the dir is writable).
    {
        const auto ro = g_root / "ro.txt";
        spit(ro, "locked\n");
        ::chmod(ro.c_str(), 0444);
        auto r = edit1(ro, "locked", "unlocked");
        CHECK(r.err);
        CHECK(slurp(ro) == "locked\n");
        ::chmod(ro.c_str(), 0644);
    }

    // Hard links: editing one name must not split it from the other.
    {
        const auto h1 = g_root / "h1.txt", h2 = g_root / "h2.txt";
        spit(h1, "shared\n");
        fs::remove(h2);
        fs::create_hard_link(h1, h2);
        CHECK(!edit1(h1, "shared", "both").err);
        CHECK(slurp(h1) == "both\n");
        CHECK(slurp(h2) == "both\n");
        CHECK(fs::hard_link_count(h1) == 2);
    }

    // Empty file: a miss, never a crash or a write.
    {
        const auto e0 = g_root / "empty.txt";
        spit(e0, "");
        CHECK(edit1(e0, "x", "y").err);
        CHECK(slurp(e0).empty());
    }

    // Non-UTF-8 bytes (latin-1) are edited byte-exactly, not "sanitised".
    {
        const auto l1 = g_root / "latin1.txt";
        const std::string body = "caf\xE9 = 1;\nna\xEFve = 2;\n";
        spit(l1, body);
        CHECK(!edit1(l1, "= 2", "= 3").err);
        CHECK(slurp(l1) == "caf\xE9 = 1;\nna\xEFve = 3;\n");
    }

    // 300 edits in one call, each exact: all land, in order.
    {
        const auto many = g_root / "many.txt";
        std::string body, want;
        Json arr = Json::array();
        for (int i = 0; i < 300; ++i) {
            body += "k" + std::to_string(i) + " = old;\n";
            want += "k" + std::to_string(i) + " = new" + std::to_string(i) + ";\n";
            arr.push_back({{"old_text", "k" + std::to_string(i) + " = old;"},
                           {"new_text", "k" + std::to_string(i) + " = new" + std::to_string(i) + ";"}});
        }
        spit(many, body);
        auto r = edits(many, arr);
        CHECK(!r.err);
        CHECK(slurp(many) == want);
    }

    // A minified 900 KB single-line file: exact edit, and a MISS, both quick
    // (a line-level matcher that falls back to char-level edit distance on a
    // 900 KB "line" would be quadratic).
    {
        const auto mini = g_root / "min.js";
        std::string body;
        while (body.size() < 900'000) body += "function a" + std::to_string(body.size()) + "(x){return x+1};";
        body += "var MARK=1;";
        spit(mini, body);
        auto r = edit1(mini, "var MARK=1;", "var MARK=2;");
        CHECK(!r.err);
        CHECK(r.ms < 250);
        (void)call("read", {{"path", mini.string()}, {"limit", 1}});
        auto m = edit1(mini, "function nope(q){return q*2};", "x");
        CHECK(m.err);
        CHECK(m.ms < 1000);
        std::printf("  minified 900 KB line: exact %.1f ms, miss %.1f ms\n", r.ms, m.ms);
    }

    // A 200 KB old_text that is absent: refused quickly, nothing written.
    {
        const auto f2 = g_root / "big_needle.txt";
        spit(f2, big_source(3000, 3));
        std::string needle;
        while (needle.size() < 200'000) needle += "zzz unrelated text " + std::to_string(needle.size()) + "\n";
        const auto before = slurp(f2);
        auto r = edit1(f2, needle, "x");
        CHECK(r.err);
        CHECK(slurp(f2) == before);
        CHECK(r.ms < 1000);
        std::printf("  200 KB absent old_text: %.1f ms\n", r.ms);
    }

    // Differential: random unique-substring edits vs a reference replace.
    std::mt19937 rng(7);
    for (int round = 0; round < 300; ++round) {
        std::string body;
        const int n = 5 + static_cast<int>(rng() % 40);
        for (int i = 0; i < n; ++i)
            body += "line_" + std::to_string(i) + "_" + std::to_string(rng() % 97)
                  + ((rng() % 7 == 0) ? "\t\xC3\xA9" : "") + "\n";
        if (rng() % 3 == 0) body.pop_back();   // sometimes no trailing newline
        spit(f, body);
        // A unique needle: one whole line (line_<i>_ prefixes are unique).
        const int i = static_cast<int>(rng() % n);
        const std::string key = "line_" + std::to_string(i) + "_";
        const auto at = body.find(key);
        auto eol = body.find('\n', at);
        if (eol == std::string::npos) eol = body.size();
        const std::string old = body.substr(at, eol - at);
        const std::string neu = (rng() % 4 == 0) ? std::string{} : "NEW_" + std::to_string(rng());
        std::string want = body;
        want.replace(at, old.size(), neu);
        auto r = edit1(f, old, neu);
        if (r.err || slurp(f) != want) {
            ++fails;
            std::printf("FAIL differential round %d: %s\n", round, r.text.substr(0, 200).c_str());
            break;
        }
    }
}

// ── speed ────────────────────────────────────────────────────────────────
struct Timing { const char* label; double ms; };
std::vector<Timing> speed(bool verbose) {
    std::vector<Timing> out;
    const auto big = g_root / "big.cpp";
    const std::string src = big_source(30000, 1);   // ~1 MiB cap is 1048576
    const auto med = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    auto run = [&](const char* label, auto&& fn, int reps = 9) {
        if (const char* only = std::getenv("EDIT_BENCH_ONLY"); only && std::string(label).find(only) == std::string::npos)
            return;
        if (std::getenv("EDIT_BENCH_ONLY")) reps *= 20;
        std::vector<double> ms;
        for (int i = 0; i < reps; ++i) { spit(big, src); ms.push_back(fn().ms); }
        out.push_back({label, med(ms)});
        if (verbose) std::printf("  %-52s %8.2f ms\n", label, out.back().ms);
    };
    std::printf("  (file: %zu bytes, 30000 lines)\n", src.size());
    run("exact, 1 line, middle of 30k-line file", [&] {
        return edit1(big, "auto v15000 = ", "auto w15000 = ");
    });
    run("exact, 3-line block", [&] {
        const auto a = src.find("auto v20000"); const auto b = src.find("auto v20003");
        const auto s = src.substr(a, b - a);
        return edit1(big, s, "// replaced\n");
    });
    run("20 edits in one call", [&] {
        Json arr = Json::array();
        for (int i = 0; i < 20; ++i)
            arr.push_back({{"old_text", "auto v" + std::to_string(1000 + i * 1400) + " ="},
                           {"new_text", "auto z" + std::to_string(i) + " ="}});
        return edits(big, arr);
    });
    run("replace_all, ~30k hits", [&] {
        return edit1(big, "compute(", "evaluate(", {{"replace_all", true}});
    }, 5);
    run("indent-drifted 4-line block (fuzzy path)", [&] {
        const auto a = src.find("auto v25000"); const auto b = src.find("auto v25004");
        std::string s = src.substr(a, b - a), t;
        for (std::size_t i = 0; i < s.size(); ++i) { t += s[i]; if (s[i] == '\n' && i + 1 < s.size()) t += "  "; }
        return edit1(big, t, "// drifted\n");
    }, 5);
    run("typo in a 3-line block (fuzzy path)", [&] {
        const auto a = src.find("auto v12000"); const auto b = src.find("auto v12003");
        std::string s = src.substr(a, b - a);
        s[s.find("compute")] = 'k';
        return edit1(big, s, "// typo fixed\n");
    }, 5);
    run("MISS: text absent, 1 line (worst case)", [&] {
        return edit1(big, "this line is nowhere in the file at all", "x");
    }, 5);
    run("MISS: text absent, 10 lines (worst case)", [&] {
        std::string s;
        for (int i = 0; i < 10; ++i) s += "    nothing_like_this_" + std::to_string(i) + "();\n";
        return edit1(big, s, "x");
    }, 5);
    run("MISS: text absent, 150 lines (worst case)", [&] {
        std::string s;
        for (int i = 0; i < 150; ++i) s += "    unrelated_statement_" + std::to_string(i * 7) + "(x, y);\n";
        return edit1(big, s, "x");
    }, 3);
    run("typo in a 120-line block (fuzzy path)", [&] {
        const auto a = src.find("auto v7000 "); const auto b = src.find("auto v7120 ");
        std::string s = src.substr(a, b - a);
        s[s.find("compute", s.size() / 2)] = 'k';
        return edit1(big, s, "// big block fixed\n");
    }, 3);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const bool bench_only = argc > 1 && std::string(argv[1]) == "--bench";
    g_root = fs::temp_directory_path() / ("edit_stress_" + std::to_string(::getpid()));
    fs::remove_all(g_root);
    fs::create_directories(g_root);
    HostServices svc;
    svc.files = std::make_shared<mcp::test::DiskFiles>();
    svc.state = mcp::test::state_at(g_root);
    svc.exec  = std::make_shared<mcp::test::PopenExec>();
    g_p = make_provider(svc, ToolsetConfig{}, "local");

    if (!bench_only) robustness();
    std::printf("edit speed:\n");
    const auto t = speed(true);
    // Budgets, generous for CI noise: an edit is interactive.
    if (!bench_only)
        for (const auto& x : t)
            if (x.ms > 250.0) { ++fails; std::printf("FAIL slow: %s took %.1f ms\n", x.label, x.ms); }

    fs::remove_all(g_root);
    if (fails == 0) std::printf("edit_stress: all passed\n");
    return fails == 0 ? 0 : 1;
}
