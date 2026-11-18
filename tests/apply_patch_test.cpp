// SPDX-License-Identifier: Apache-2.0
//
// apply_patch_test.cpp — the apply_patch tool: unified-diff application with
// fuzzy hunk location, driven through make_provider(). Pins the contract:
// clean multi-hunk apply, fuzzy apply after the file drifts, ATOMICITY (a bad
// hunk writes nothing), ambiguity rejection, and the FileChange carry.

#include <chrono>
#include <thread>
#include "support/disk_files.hpp"
#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/toolset.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/local.hpp>

#include "agtest.hpp"
#include "test_state.hpp"
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
static void wr(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), (std::streamsize)s.size());
}
static std::string rd(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static bool has(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

TEST_CASE("apply_patch") {
    auto root = fs::temp_directory_path() / ("mcp_patch_test_" + std::to_string(mcp_getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    auto ws_state = mcp::test::state_at(root);
    auto prev = fs::current_path();
    fs::current_path(root);

    HostServices svc;

    svc.files = std::make_shared<mcp::test::DiskFiles>();
    svc.state = ws_state;
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    const std::string base =
        "line one\n"
        "line two\n"
        "line three\n"
        "line four\n"
        "line five\n";

    // ── clean single-hunk apply + FileChange carried ─────────────────────
    {
        wr(root / "a.txt", base);
        auto a = obj();
        a["path"] = (root / "a.txt").string();
        a["patch"] =
            "@@ -1,3 +1,3 @@\n"
            " line one\n"
            "-line two\n"
            "+line TWO changed\n"
            " line three\n";
        auto r = call(*provider, "apply_patch", a);
        check(!r.is_error, "clean patch applies");
        check(has(rd(root / "a.txt"), "line TWO changed"), "removed→added line landed");
        check(has(rd(root / "a.txt"), "line one") &&
              has(rd(root / "a.txt"), "line five"), "context untouched");
        check(r.effects.has(Effect::WriteFs), "apply_patch is WriteFs");
        auto ch = only_change(r);
        check(ch.has_value(), "carries a FileChange for the diff-review UI");
        check(ch && ch->added >= 1 && ch->removed >= 1, "FileChange has add/remove counts");
    }

    // ── multi-hunk apply, both hunks land ────────────────────────────────
    {
        wr(root / "b.txt", base);
        auto a = obj();
        a["path"] = (root / "b.txt").string();
        a["patch"] =
            "@@ -1,2 +1,2 @@\n"
            " line one\n"
            "-line two\n"
            "+HUNK1\n"
            "@@ -4,2 +4,2 @@\n"
            " line four\n"
            "-line five\n"
            "+HUNK2\n";
        auto r = call(*provider, "apply_patch", a);
        check(!r.is_error, "multi-hunk patch applies");
        auto out = rd(root / "b.txt");
        check(has(out, "HUNK1") && has(out, "HUNK2"), "both hunks landed");
        check(!has(out, "line two") && !has(out, "line five"), "both removals took");
    }

    // ── fuzzy: file DRIFTED (extra leading lines shift line numbers) ─────
    {
        wr(root / "c.txt",
           "// a new header comment\n"
           "// another new line\n"
           + base);   // base now starts at line 3, but @@ says line 1
        auto a = obj();
        a["path"] = (root / "c.txt").string();
        a["patch"] =
            "@@ -3,3 +3,3 @@\n"
            " line two\n"
            "-line three\n"
            "+line THREE\n"
            " line four\n";
        auto r = call(*provider, "apply_patch", a);
        check(!r.is_error, "patch applies despite drifted line numbers");
        check(has(rd(root / "c.txt"), "line THREE"),
              "fuzzy located the hunk past the drift");
        check(has(rd(root / "c.txt"), "// a new header comment"),
              "the drifted-in lines are preserved");
    }

    // ── ATOMICITY: one good hunk + one bad hunk → NOTHING written ────────
    {
        wr(root / "d.txt", base);
        auto before = rd(root / "d.txt");
        auto a = obj();
        a["path"] = (root / "d.txt").string();
        a["patch"] =
            "@@ -1,2 +1,2 @@\n"
            " line one\n"
            "-line two\n"
            "+GOOD\n"
            "@@ -4,2 +4,2 @@\n"
            " line four\n"
            "-this line does not exist anywhere\n"   // bad hunk
            "+NOPE\n";
        auto r = call(*provider, "apply_patch", a);
        check(r.is_error, "a patch with an unmatchable hunk is an error");
        check(rd(root / "d.txt") == before,
              "ATOMIC: the good hunk was NOT written when a sibling hunk failed");
        check(!has(rd(root / "d.txt"), "GOOD"), "no partial application");
    }

    // ── no-op patch: already in the target state ─────────────────────────
    {
        wr(root / "e.txt", base);
        auto a = obj();
        a["path"] = (root / "e.txt").string();
        // A hunk whose - and + sides are identical context → no change.
        a["patch"] =
            "@@ -1,3 +1,3 @@\n"
            " line one\n"
            " line two\n"
            " line three\n";
        auto r = call(*provider, "apply_patch", a);
        check(!r.is_error, "no-op patch is not an error");
        check(has(r.text, "no change"), "no-op is reported as such");
    }

    // ── error paths ──────────────────────────────────────────────────────
    {
        auto a = obj();               // missing patch
        a["path"] = (root / "a.txt").string();
        check(call(*provider, "apply_patch", a).is_error, "missing patch is an error");

        auto b = obj();               // no @@ hunks
        b["path"] = (root / "a.txt").string();
        b["patch"] = "this is not a diff at all\n";
        check(call(*provider, "apply_patch", b).is_error, "a patch with no hunks is an error");

        auto c = obj();               // nonexistent file
        c["path"] = (root / "nope.txt").string();
        c["patch"] = "@@ -1,1 +1,1 @@\n-x\n+y\n";
        check(call(*provider, "apply_patch", c).is_error, "patching a missing file is an error");

        auto d = obj();               // pure insertion (no context) → rejected
        wr(root / "f.txt", base);
        d["path"] = (root / "f.txt").string();
        d["patch"] = "@@ -0,0 +1,1 @@\n+inserted with no context\n";
        check(call(*provider, "apply_patch", d).is_error,
              "a context-less pure insertion is rejected (can't place it)");
    }

    // ── DoS BOUND: hunk count is capped. Each hunk runs a full fuzzy_find
    //    over the file; the per-hunk cost is bounded but the COUNT was not, so
    //    a patch with thousands of hunks turned a bounded search into a
    //    minutes-long apply. A patch far over the cap must be REJECTED fast,
    //    not ground through. (Well over any real edit to a small file.)
    {
        wr(root / "many.txt", base);
        std::string patch;
        for (int i = 0; i < 5000; ++i)
            patch += "@@ -1,1 +1,1 @@\n-line one\n+changed\n";
        auto a = obj();
        a["path"]  = (root / "many.txt").string();
        a["patch"] = patch;
        const auto t0 = std::chrono::steady_clock::now();
        auto r = call(*provider, "apply_patch", a);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        check(r.is_error, "a patch with thousands of hunks is rejected");
        check(has(r.text, "hunks") || has(r.text, "cap"),
              "rejection names the hunk cap");
        check(ms < 1'000, "over-cap patch is rejected fast, not ground through");
        check(rd(root / "many.txt") == base, "ATOMIC: nothing written on reject");
    }

    // ── run_edit Match reporting / refusals ──────────────────────────────
    auto check = [](bool ok, const char* what) { CHECK_MESSAGE(ok, std::string(what)); };
    auto edit1 = [&](const fs::path& p, std::string o, std::string n,
                     bool all = false) {
        auto a = obj();
        a["path"] = p.string();
        auto e = obj(); e["old_text"] = o; e["new_text"] = n;
        if (all) e["replace_all"] = true;
        a["edits"] = mcp::Json::array({e});
        return call(*provider, "edit", a);
    };
    {
        wr(root / "m.txt", base);
        auto r = edit1(root / "m.txt", "line two\n", "line 2\n");
        check(!r.is_error && has(r.text, "Match:") && has(r.text, "exact"),
              "exact edit reports its strategy");
        wr(root / "m2.txt", "fn() {\n        a = 1;\n        b = 2;\n}\n");
        auto r2 = edit1(root / "m2.txt", "    a = 1;\n    b = 2;\n", "    a = 3;\n    b = 2;\n");
        check(!r2.is_error && has(r2.text, "\xe2\x9a\xa0"),
              "indent-relative edit carries a warning");
        wr(root / "m3.txt", base);
        auto r3 = edit1(root / "m3.txt", "line  tw0\n", "X\n", true);
        check(r3.is_error && rd(root / "m3.txt") == base, "replace_all refuses a near-miss");
        auto a = obj();
        a["path"] = (root / "m3.txt").string();
        auto e1 = obj(); e1["old_text"] = "line one\nline two\n"; e1["new_text"] = "A\n";
        auto e2 = obj(); e2["old_text"] = "line two\nline three\n"; e2["new_text"] = "B\n";
        a["edits"] = mcp::Json::array({e1, e2});
        auto r4 = call(*provider, "edit", a);
        check(has(r4.text, "1 failed") && !has(rd(root / "m3.txt"), "B\n"),
              "overlapping edit refused");
    }

    // ── stale + fuzzy refused, stale + exact applied ─────────────────────
    {
        wr(root / "s.txt", base);
        auto rr = obj(); rr["path"] = (root / "s.txt").string();
        (void)call(*provider, "read", rr);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        wr(root / "s.txt", base + "line six\n");
        fs::last_write_time(root / "s.txt", fs::file_time_type::clock::now() + std::chrono::seconds(5));
        auto a = obj(); a["path"] = (root / "s.txt").string();
        a["patch"] = "@@ -1,2 +1,2 @@\n line onee\n-line two\n+FUZZ\n line three\n";
        auto r = call(*provider, "apply_patch", a);
        check(r.is_error && !has(rd(root / "s.txt"), "FUZZ"), "stale + fuzzy refused");
        a["patch"] = "@@ -1,2 +1,2 @@\n line one\n-line two\n+EXACT\n line three\n";
        auto r2 = call(*provider, "apply_patch", a);
        check(!r2.is_error && has(rd(root / "s.txt"), "EXACT"), "stale + exact applied");
    }

    // ── CRLF dominance, BOM, no-newline-at-EOF ───────────────────────────
    {
        wr(root / "crlf.txt", "a\r\nb\r\nc\nd\r\ne\r\n");
        auto a = obj(); a["path"] = (root / "crlf.txt").string();
        a["patch"] = "@@ -1,2 +1,2 @@\n a\n-b\n+B\n";
        auto r = call(*provider, "apply_patch", a);
        check(!r.is_error && rd(root / "crlf.txt") == "a\r\nB\r\nc\nd\r\ne\r\n",
              "CRLF-dominant file keeps CRLF");
        check(has(r.text, "Match:"), "apply_patch reports match strategy");

        wr(root / "bom.txt", "\xEF\xBB\xBFx\ny\n");
        a["path"] = (root / "bom.txt").string();
        a["patch"] = "@@ -1,2 +1,2 @@\n x\n-y\n+Y\n";
        check(!call(*provider, "apply_patch", a).is_error
              && rd(root / "bom.txt") == "\xEF\xBB\xBFx\nY\n", "BOM preserved");

        wr(root / "eol.txt", "x\ny");
        a["path"] = (root / "eol.txt").string();
        a["patch"] = "@@ -1,2 +1,2 @@\n x\n-y\n\\ No newline at end of file\n+Y\n";
        auto r3 = call(*provider, "apply_patch", a);
        check(!r3.is_error && rd(root / "eol.txt") == "x\nY\n", "no-EOL marker honoured");
        wr(root / "eol2.txt", "x\ny");
        a["path"] = (root / "eol2.txt").string();
        a["patch"] = "@@ -1,2 +1,2 @@\n x\n-y\n+Y\n";
        check(!call(*provider, "apply_patch", a).is_error
              && rd(root / "eol2.txt") == "x\nY", "trailing-newline state preserved");
    }

    // ── multi-file: V4A + unified ────────────────────────────────────────
    {
        wr(root / "u1.txt", base); wr(root / "mv.txt", base); wr(root / "del.txt", "bye\n");
        auto a = obj();
        a["patch"] = "*** Begin Patch\n"
                     "*** Add File: new.txt\n+hello\n+world\n"
                     "*** Update File: u1.txt\n@@\n line one\n-line two\n+LINE2\n"
                     "*** Update File: mv.txt\n*** Move to: moved.txt\n@@\n line four\n-line five\n+FIVE\n"
                     "*** Delete File: del.txt\n"
                     "*** End Patch\n";
        auto r = call(*provider, "apply_patch", a);
        check(!r.is_error, "V4A applies");
        check(rd(root / "new.txt") == "hello\nworld\n", "V4A add");
        check(has(rd(root / "u1.txt"), "LINE2"), "V4A update");
        check(!fs::exists(root / "mv.txt") && has(rd(root / "moved.txt"), "FIVE"), "V4A move");
        check(!fs::exists(root / "del.txt"), "V4A delete");
        check(has(r.text, "Added") && has(r.text, "Moved") && has(r.text, "Deleted"), "summary");

        wr(root / "f1.txt", base); wr(root / "f2.txt", base);
        a["patch"] = "*** Begin Patch\n"
                     "*** Update File: f1.txt\n@@\n line one\n-line two\n+CHANGED\n"
                     "*** Add File: added.txt\n+x\n"
                     "*** Update File: f2.txt\n@@\n-no such line here at all\n+Z\n"
                     "*** End Patch\n";
        auto r2 = call(*provider, "apply_patch", a);
        check(r2.is_error && has(r2.text, "f2.txt"), "failing op named");
        check(rd(root / "f1.txt") == base && !fs::exists(root / "added.txt"), "nothing written");

        a["patch"] = "--- a/f1.txt\n+++ b/f1.txt\n@@ -1,2 +1,2 @@\n line one\n-line two\n+U1\n"
                     "--- /dev/null\n+++ b/uadd.txt\n@@ -0,0 +1 @@\n+fresh\n";
        auto r3 = call(*provider, "apply_patch", a);
        check(!r3.is_error && has(rd(root / "f1.txt"), "U1")
              && rd(root / "uadd.txt") == "fresh\n", "multi-file unified diff");
    }

    fs::current_path(prev);
    fs::remove_all(root);
}
