// jq_conformance.cpp — json_query's jq engine vs the real `jq` binary.
//
// Runs every query in jq_conformance/queries.txt through util::jq and
// through `jq -c`, and fails on the first byte of difference (an error must
// be an error in both). The corpus is the jq most agents actually write:
// paths, select/map, `..`, `//`, reduce, regex, @formats, assignment.
//   mcp_jq_conformance <input.json> <queries.txt> [jq-binary]
// Exit 0 = all match; 77 = no jq binary (ctest SKIP).
#include "../src/tools/util/jq.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
int main() { return 77; }
#else
#include <sys/wait.h>

namespace {
std::string shell_quote(const std::string& s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
}

// {ok, stdout}. stderr is discarded; a nonzero exit means "error".
std::pair<bool, std::string> run_jq(const std::string& jq, const std::string& in, const std::string& q) {
    const std::string cmd = shell_quote(jq) + " -c " + shell_quote(q) + " " + shell_quote(in) + " 2>/dev/null";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return {false, {}};
    std::string out;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, p)) > 0;) out.append(buf, n);
    const int st = ::pclose(p);
    return {WIFEXITED(st) && WEXITSTATUS(st) == 0, out};
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: mcp_jq_conformance input.json queries.txt [jq]\n"; return 2; }
    const std::string jq = argc > 3 ? argv[3] : "jq";
    if (run_jq(jq, argv[1], ".").second.empty()) {
        std::cout << "no usable `" << jq << "` binary; skipping\n";
        return 77;
    }
    std::ifstream in(argv[1]);
    const auto doc = nlohmann::ordered_json::parse(in);
    std::ifstream qs(argv[2]);
    int pass = 0, fail = 0;
    for (std::string q; std::getline(qs, q);) {
        if (q.empty() || q[0] == '#') continue;
        const auto ours = mcp::tools::util::jq::run(q, doc);
        const auto [ok, want] = run_jq(jq, argv[1], q);
        std::string got;
        if (ours) for (const auto& v : *ours) got += v.dump() + "\n";
        if (ours.has_value() == ok && (!ok || got == want)) { ++pass; continue; }
        ++fail;
        std::cout << "MISMATCH: " << q << "\n  ours: " << (ours ? got : ours.error() + "\n")
                  << "  jq:   " << (ok ? want : "(error)\n");
    }
    std::cout << pass << " match real jq, " << fail << " differ\n";
    return fail == 0 ? 0 : 1;
}
#endif
