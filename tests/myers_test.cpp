// myers_test — the Myers diff reproduces `b` exactly and is MINIMAL (its
// edit count equals n + m - 2*LCS) across random and adversarial inputs.
#include <mcp/tools/util/myers.hpp>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {
int fails = 0;
void check(bool ok, const char* what, int seed) {
    if (!ok) { std::printf("FAIL %s (seed %d)\n", what, seed); ++fails; }
}
int lcs(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::vector<std::vector<int>> d(a.size() + 1, std::vector<int>(b.size() + 1, 0));
    for (std::size_t i = 1; i <= a.size(); ++i)
        for (std::size_t j = 1; j <= b.size(); ++j)
            d[i][j] = a[i - 1] == b[j - 1] ? d[i - 1][j - 1] + 1 : std::max(d[i - 1][j], d[i][j - 1]);
    return d[a.size()][b.size()];
}
void run(const std::vector<std::string>& a, const std::vector<std::string>& b, int seed) {
    const auto es = myers::diff(a, b);
    std::vector<std::string> rebuilt;
    std::size_t ai = 0, bi = 0;
    int changes = 0;
    bool order = true;
    for (const auto& e : es) {
        if (e.k == myers::Edit::Keep) {
            order = order && e.a_idx == (int)ai && e.b_idx == (int)bi && a[ai] == b[bi];
            rebuilt.push_back(a[ai]); ++ai; ++bi;
        } else if (e.k == myers::Edit::Del) {
            order = order && e.a_idx == (int)ai; ++ai; ++changes;
        } else {
            order = order && e.b_idx == (int)bi;
            rebuilt.push_back(b[bi]); ++bi; ++changes;
        }
    }
    check(order && ai == a.size() && bi == b.size(), "indices walk both sides in order", seed);
    check(rebuilt == b, "applying the script yields b", seed);
    check(changes == (int)(a.size() + b.size()) - 2 * lcs(a, b), "script is minimal", seed);
}
}  // namespace

int main() {
    std::mt19937 rng(12345);
    for (int seed = 0; seed < 3000; ++seed) {
        const int alpha = 1 + (int)(rng() % 6);
        auto gen = [&](int len) {
            std::vector<std::string> v;
            for (int i = 0; i < len; ++i) v.push_back(std::string(1, char('a' + rng() % alpha)));
            return v;
        };
        auto a = gen((int)(rng() % 40));
        auto b = (rng() % 3 == 0) ? gen((int)(rng() % 40)) : a;
        // mutate b
        for (int k = 0, n = (int)(rng() % 6); k < n; ++k) {
            const int op = (int)(rng() % 3);
            if (op == 0 && !b.empty()) b.erase(b.begin() + (long)(rng() % b.size()));
            else if (op == 1) b.insert(b.begin() + (long)(b.empty() ? 0 : rng() % (b.size() + 1)),
                                       std::string(1, char('a' + rng() % alpha)));
            else if (!b.empty()) b[rng() % b.size()] = std::string(1, char('a' + rng() % alpha));
        }
        run(a, b, seed);
    }
    run({}, {}, -1);
    run({"x"}, {}, -2);
    run({}, {"x"}, -3);
    run({"", ""}, {""}, -4);
    // A one-line edit in a big file: must be one Del + one Ins.
    std::vector<std::string> big;
    for (int i = 0; i < 5000; ++i) big.push_back("line " + std::to_string(i));
    auto big2 = big;
    big2[2500] = "changed";
    const auto es = myers::diff(big, big2);
    int ch = 0;
    for (const auto& e : es) ch += e.k != myers::Edit::Keep;
    check(ch == 2, "one-line edit in 5000 lines is 2 changes", -5);

    // window(): the lines it leaves out are identical on both sides, and the
    // lines it keeps are exactly the full split's lines at [off, off+len).
    std::mt19937 wr(99);
    auto join = [](const std::vector<std::string>& v, bool trailing_nl) {
        std::string s;
        for (std::size_t i = 0; i < v.size(); ++i) { s += v[i]; if (i + 1 < v.size() || trailing_nl) s += '\n'; }
        return s;
    };
    for (int seed = 0; seed < 4000; ++seed) {
        std::vector<std::string> la;
        const int nlines = (int)(wr() % 30);
        for (int i = 0; i < nlines; ++i) la.push_back(std::string((wr() % 3), char('a' + wr() % 3)));
        auto lb = la;
        for (int k = 0, m = (int)(wr() % 4); k < m; ++k) {
            const int op = (int)(wr() % 3);
            if (op == 0 && !lb.empty()) lb.erase(lb.begin() + (long)(wr() % lb.size()));
            else if (op == 1) lb.insert(lb.begin() + (long)(lb.empty() ? 0 : wr() % (lb.size() + 1)), std::string(1, char('a' + wr() % 3)));
            else if (!lb.empty()) lb[wr() % lb.size()] += 'x';
        }
        const std::string A = join(la, wr() % 2), B = join(lb, wr() % 2);
        const auto fa = myers::detail::split_views(A), fb = myers::detail::split_views(B);
        const int ctx = (int)(wr() % 4);
        const auto w = myers::window(A, B, ctx);
        bool ok = w.off >= 0 && (std::size_t)w.off + w.a.size() <= fa.size()
                  && (std::size_t)w.off + w.b.size() <= fb.size();
        if (ok) {
            for (int i = 0; i < w.off; ++i) ok = ok && fa[i] == fb[i];
            for (std::size_t i = 0; i < w.a.size(); ++i) ok = ok && w.a[i] == fa[w.off + i];
            for (std::size_t i = 0; i < w.b.size(); ++i) ok = ok && w.b[i] == fb[w.off + i];
            const std::size_t ta = fa.size() - w.off - w.a.size(), tb = fb.size() - w.off - w.b.size();
            ok = ok && ta == tb;
            for (std::size_t i = 0; ok && i < ta; ++i) ok = ok && fa[fa.size() - 1 - i] == fb[fb.size() - 1 - i];
            // And the edit count over the window equals the full minimal count.
            int full = 0, part = 0;
            for (const auto& e : myers::diff(fa, fb)) full += e.k != myers::Edit::Keep;
            for (const auto& e : myers::diff(w.a, w.b)) part += e.k != myers::Edit::Keep;
            ok = ok && full == part;
        }
        check(ok, "window is exact and keeps the minimal edit count", seed);
        if (!ok) break;
    }
    if (fails == 0) std::printf("myers_test: all passed\n");
    return fails == 0 ? 0 : 1;
}
