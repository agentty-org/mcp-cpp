// shell_bound_test — head_lines / tail_lines bound what reaches the MODEL
// without bounding what the user's terminal card shows.
//
// Why this exists: models pipe `| head -20` / `| tail -20` to keep output
// out of their context. That filters at the wrong layer — the pipe discards
// the rest before the card ever sees it, so the HUMAN loses output they were
// watching in order to save the model's context. These parameters do the
// slicing at the boundary instead.
//
// Drives the REAL provider, not the helper, so the schema/parse/apply path
// is covered end to end.

#include "test_state.hpp"
#include "test_exec.hpp"
#include "support/disk_files.hpp"
#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/util/fs_helpers.hpp>
#include <mcp/cap/local.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace mcp::tools;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    auto root = fs::temp_directory_path() / ("mcp_shell_bound_" + std::to_string(::getpid()));
    fs::create_directories(root);
    auto ws_state = mcp::test::state_at(root);

    HostServices svc;

    svc.files = std::make_shared<mcp::test::DiskFiles>();

    svc.state = ws_state;

    svc.exec  = std::make_shared<mcp::test::PopenExec>();
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    auto run = [&](mcp::Json a) {
        return provider->execute(mcp::cap::Request{"shell", std::move(a)});
    };
    // 30 lines: line1 … line30.
    const char* seq = "i=1; while [ $i -le 30 ]; do echo line$i; i=$((i+1)); done";

    // ── tail_lines keeps the END, which is where a build failure lives ──
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = seq;
        a["tail_lines"] = 3;
        auto r = run(a);
        CHECK(!r.is_error);
        CHECK(has(r.text, "line30"));
        CHECK(has(r.text, "line28"));
        CHECK(!has(r.text, "line1\n"));   // the head is gone
        CHECK(!has(r.text, "line15"));
        // The elision is HONEST about how much it dropped — a silent slice
        // would let the model conclude the command only printed 3 lines.
        CHECK(has(r.text, "27 lines elided"));
        // …and says the user still has it, so the model doesn't re-run the
        // command unbounded just to "show" the user something they can see.
        CHECK(has(r.text, "terminal card"));
    }

    // ── head_lines keeps the START ──────────────────────────────────────
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = seq;
        a["head_lines"] = 2;
        auto r = run(a);
        CHECK(!r.is_error);
        CHECK(has(r.text, "line1"));
        CHECK(has(r.text, "line2"));
        CHECK(!has(r.text, "line30"));
        CHECK(has(r.text, "28 lines elided"));
    }

    // ── both ends, middle elided (the build-log shape) ──────────────────
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = seq;
        a["head_lines"] = 2;
        a["tail_lines"] = 2;
        auto r = run(a);
        CHECK(!r.is_error);
        CHECK(has(r.text, "line1"));
        CHECK(has(r.text, "line30"));
        CHECK(!has(r.text, "line15"));
        CHECK(has(r.text, "26 lines elided"));
    }

    // ── a bound LARGER than the output elides nothing ───────────────────
    // Reporting "0 lines elided" on complete output would be a lie of
    // emphasis; the text must come back untouched.
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = "echo a; echo b; echo c";
        a["tail_lines"] = 50;
        auto r = run(a);
        CHECK(!r.is_error);
        CHECK(has(r.text, "a"));
        CHECK(has(r.text, "c"));
        CHECK(!has(r.text, "elided"));
    }

    // ── absent parameters change nothing ────────────────────────────────
    {
        mcp::Json a = mcp::Json::object();
        a["command"] = seq;
        auto r = run(a);
        CHECK(!r.is_error);
        CHECK(has(r.text, "line1"));
        CHECK(has(r.text, "line30"));
        CHECK(!has(r.text, "elided"));
    }

    // ── soft timeout: the command becomes a session and finishes there ──
    auto procs = [&](const char* name, mcp::Json a) {
        return provider->execute(mcp::cap::Request{name, std::move(a)});
    };
    {
        auto r = run({{"command", "sleep 3; echo done"}, {"timeout", 1}});
        CHECK(!r.is_error);
        CHECK(has(r.text, "still running as background session proc-"));
        CHECK(has(r.text, "Full log: "));
        auto p = r.text.find("session proc-");
        std::string id = r.text.substr(p + 8, r.text.find(' ', p + 8) - (p + 8));
        bool done = false, exit0 = false;
        for (int i = 0; i < 10 && !(done && exit0); ++i) {
            auto q = procs("process_poll", {{"id", id}, {"wait_ms", 1000}});
            if (has(q.text, "done")) done = true;
            if (has(q.text, "exit 0") || has(q.text, "code 0") || has(q.text, "exited (0)")) exit0 = true;
            if (has(q.text, "exited") && done) exit0 = exit0 || !has(q.text, "exit 1");
        }
        CHECK(done);
        CHECK(exit0);
        procs("process_stop", {{"id", id}});
    }
    // ── on_timeout=kill: no session is left behind ──────────────────────
    {
        auto r = run({{"command", "echo quick"}, {"timeout", 1}, {"on_timeout", "kill"}});
        CHECK(!r.is_error);
        CHECK(has(r.text, "quick"));
        CHECK(!has(r.text, "background session"));
        auto bad = run({{"command", "true"}, {"on_timeout", "maybe"}});
        CHECK(bad.is_error);
    }
    // ── a quick command is unchanged and leaves no session ─────────────
    {
        auto r = run({{"command", "echo fast"}});
        CHECK(has(r.text, "fast"));
        CHECK(!has(r.text, "background"));
    }
    // ── cwd persists, and is clamped to the workspace ──────────────────
    {
        fs::create_directories(root / "sub");
        auto r = run({{"command", "cd '" + (root / "sub").string() + "'"}});
        CHECK(has(r.text, "[cwd is now "));
        auto p = run({{"command", "pwd"}});
        CHECK(has(p.text, "/sub"));
        auto out = run({{"command", "cd /"}});
        CHECK(!has(out.text, "[cwd is now /]"));
        auto p2 = run({{"command", "pwd"}});
        CHECK(has(p2.text, "/sub"));
        auto ex = run({{"command", "pwd"}, {"cd", root.string()}});
        CHECK(!has(ex.text, "/sub"));
        run({{"command", "cd .."}});
    }
    // ── prompt hint ─────────────────────────────────────────────────────
    {
        auto r = run({{"command", "echo 'Proceed? [y/N]'; exit 1"}});
        CHECK(has(r.text, "waiting for input"));
        auto ok = run({{"command", "echo fine; exit 1"}});
        CHECK(!has(ok.text, "waiting for input"));
    }
    // ── pipe hint ───────────────────────────────────────────────────────
    {
        auto r = run({{"command", "sleep 3 | tail -5"}, {"timeout", 1}});
        CHECK(has(r.text, "buffered by the pipe"));
        auto p = r.text.find("session proc-");
        if (p != std::string::npos)
            procs("process_stop", {{"id", r.text.substr(p + 8, r.text.find(' ', p + 8) - (p + 8))}});
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures == 0) std::puts("shell_bound_test: OK");
    return g_failures == 0 ? 0 : 1;
}
