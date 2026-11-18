// SPDX-License-Identifier: Apache-2.0
#pragma once
// myers.hpp — Myers' O((N+M)·D) line diff (the algorithm git and GNU diff
// use), header-only so agentty's diff and mcp-cpp's can share one copy.
//
// Replaces an O(N·M) LCS table. On a 2,000-line file with a one-line edit
// the table did 4M cell updates and touched 16 MB (the page faults were most
// of an `edit` call); Myers does ~2k steps. Cost scales with how much
// CHANGED, not how big the file is.
//
// Lines are interned to ints first, so every comparison is an int compare.
// The forward pass keeps one V vector per D (the standard trace), which is
// O(D·(N+M)) memory -- bounded by kMaxD below; past it the middle falls back
// to delete-all/insert-all of the remaining window, which is correct, just
// not minimal (a wholesale rewrite, where minimality buys nothing).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace myers {

struct Edit { enum K { Keep, Del, Ins } k; int a_idx, b_idx; };

namespace detail {

// Split on '\n' into views of `s`. A trailing newline yields a final empty
// line, matching the line model both diff engines already used.
inline std::vector<std::string_view> split_views(std::string_view s) {
    std::vector<std::string_view> out;
    // memchr is SIMD in every libc; the byte loop it replaced was the largest
    // single cost of an edit on a 2,000-line file.
    std::size_t lines = 1;
    for (const char* p = s.data(), *e = s.data() + s.size();
         (p = static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(e - p)))) != nullptr; ++p)
        ++lines;
    out.reserve(lines);
    const char* p = s.data();
    const char* const e = s.data() + s.size();
    while (const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(e - p)))) {
        out.emplace_back(p, static_cast<std::size_t>(nl - p));
        p = nl + 1;
    }
    out.emplace_back(p, static_cast<std::size_t>(e - p));
    return out;
}

}  // namespace detail

// Count '\n' in s, 8 bytes at a time (SWAR zero-byte detect + popcount).
inline std::size_t count_newlines(std::string_view s) noexcept {
    std::size_t n = 0, i = 0;
    constexpr std::uint64_t kOnes = 0x0101010101010101ull, kHigh = 0x8080808080808080ull;
    constexpr std::uint64_t kNl = kOnes * static_cast<unsigned char>('\n');
    for (; i + 8 <= s.size(); i += 8) {
        std::uint64_t w;
        std::memcpy(&w, s.data() + i, 8);
        w ^= kNl;                                   // '\n' bytes become 0
        const std::uint64_t z = (w - kOnes) & ~w & kHigh;   // high bit set per zero byte
        n += static_cast<std::size_t>(__builtin_popcountll(z));
    }
    for (; i < s.size(); ++i) n += s[i] == '\n';
    return n;
}

// The part of a two-buffer diff worth splitting into lines.
//
// A one-line edit to a 30,000-line file used to split BOTH whole buffers
// into lines (60,000 views) before Myers trimmed the common prefix and
// suffix back off. Trim at the BYTE level first -- a few memcmp blocks --
// widen to whole lines plus `ctx` lines of context on each side, and split
// only that. `off` is how many lines precede the window, so window line i
// is file line off+i (0-based) on both sides.
struct Window {
    std::vector<std::string_view> a, b;
    int off = 0;
};

inline Window window(std::string_view before, std::string_view after, int ctx = 3) {
    const std::size_t n = std::min(before.size(), after.size());
    constexpr std::size_t kBlock = 4096;
    std::size_t p = 0;
    while (p + kBlock <= n && std::memcmp(before.data() + p, after.data() + p, kBlock) == 0) p += kBlock;
    while (p < n && before[p] == after[p]) ++p;
    std::size_t s = 0;
    const std::size_t smax = n - p;
    while (s + kBlock <= smax
           && std::memcmp(before.data() + before.size() - s - kBlock,
                          after.data() + after.size() - s - kBlock, kBlock) == 0)
        s += kBlock;
    while (s < smax && before[before.size() - 1 - s] == after[after.size() - 1 - s]) ++s;

    // Head: back to the start of the line holding the first difference, then
    // ctx more lines. Identical bytes in both buffers, so one offset serves.
    std::size_t ws = 0;
    if (p > 0) {
        const auto nl = before.rfind('\n', p - 1);   // last '\n' at or before p-1
        ws = nl == std::string_view::npos ? 0 : nl + 1;
    }
    for (int k = 0; k < ctx && ws > 0; ++k) {
        const auto nl = ws >= 2 ? before.rfind('\n', ws - 2) : std::string_view::npos;
        ws = nl == std::string_view::npos ? 0 : nl + 1;
    }

    // Tail: the common suffix starts at before.size()-s; extend the window
    // forward to the end of that line, then ctx more lines. The '\n' bytes it
    // stops after lie inside the common suffix, so both sides agree.
    std::size_t we = before.size() - s;
    for (int k = 0; k <= ctx && we < before.size(); ++k) {
        const auto nl = before.find('\n', we);
        we = nl == std::string_view::npos ? before.size() : nl + 1;
    }
    const std::size_t tail = before.size() - we;

    Window w;
    w.off = static_cast<int>(count_newlines(before.substr(0, ws)));
    w.a = detail::split_views(before.substr(ws, we - ws));
    w.b = detail::split_views(after.substr(ws, after.size() - tail - ws));
    // A window that stops short of the end ends just after a '\n'; the empty
    // "line" split_views reports after it is not a line (the real next line
    // is in the trimmed tail).
    if (tail > 0) {
        if (!w.a.empty() && w.a.back().empty()) w.a.pop_back();
        if (!w.b.empty() && w.b.back().empty()) w.b.pop_back();
    }
    return w;
}

// Edit script turning a into b, in order, common prefix/suffix as Keep.
template <class Line>
std::vector<Edit> diff(const std::vector<Line>& a, const std::vector<Line>& b,
                       std::size_t max_d = 2048) {
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    const auto sv = [](const Line& l) { return std::string_view(l); };

    std::vector<Edit> out;
    out.reserve(static_cast<std::size_t>(std::max(n, m)) + 8);

    // Common prefix/suffix by direct comparison, BEFORE interning: a typical
    // edit leaves all but a few lines alone, and hashing 2,000 lines to find
    // that out was most of the diff's cost.
    int pre = 0;
    while (pre < n && pre < m && sv(a[static_cast<std::size_t>(pre)]) == sv(b[static_cast<std::size_t>(pre)])) {
        out.push_back({Edit::Keep, pre, pre});
        ++pre;
    }
    int ea = n, eb = m;
    while (ea > pre && eb > pre
           && sv(a[static_cast<std::size_t>(ea - 1)]) == sv(b[static_cast<std::size_t>(eb - 1)])) {
        --ea; --eb;
    }

    // Intern just the changed window, so every comparison below is an int one.
    std::unordered_map<std::string_view, int> ids;
    ids.reserve(static_cast<std::size_t>((ea - pre) + (eb - pre)));
    std::vector<int> ai(static_cast<std::size_t>(ea - pre)), bi(static_cast<std::size_t>(eb - pre));
    for (int i = pre; i < ea; ++i)
        ai[static_cast<std::size_t>(i - pre)] =
            ids.emplace(sv(a[static_cast<std::size_t>(i)]), static_cast<int>(ids.size())).first->second;
    for (int j = pre; j < eb; ++j)
        bi[static_cast<std::size_t>(j - pre)] =
            ids.emplace(sv(b[static_cast<std::size_t>(j)]), static_cast<int>(ids.size())).first->second;

    const int N = ea - pre, M = eb - pre;
    const auto A = [&](int x) { return ai[static_cast<std::size_t>(x)]; };
    const auto B = [&](int y) { return bi[static_cast<std::size_t>(y)]; };

    if (N == 0 || M == 0) {
        for (int x = 0; x < N; ++x) out.push_back({Edit::Del, pre + x, -1});
        for (int y = 0; y < M; ++y) out.push_back({Edit::Ins, -1, pre + y});
    } else {
        const int max = N + M;
        const int off = max;
        std::vector<int> v(static_cast<std::size_t>(2 * max + 2), 0);
        // trace[d] holds V over diagonals [-d, d] as it stood BEFORE step d
        // (index k + d): the backtrack at d reads only k-1 / k+1 there. A
        // window per step is O(D^2) total instead of O(D*(N+M)).
        std::vector<std::vector<int>> trace;
        int found_d = -1;
        const int dcap = static_cast<int>(std::min<std::size_t>(max_d, static_cast<std::size_t>(max)));
        for (int d = 0; d <= dcap && found_d < 0; ++d) {
            trace.emplace_back(v.begin() + (off - d), v.begin() + (off + d + 1));
            for (int k = -d; k <= d; k += 2) {
                int x;
                if (k == -d || (k != d && v[static_cast<std::size_t>(off + k - 1)]
                                             < v[static_cast<std::size_t>(off + k + 1)]))
                    x = v[static_cast<std::size_t>(off + k + 1)];          // down: insert
                else
                    x = v[static_cast<std::size_t>(off + k - 1)] + 1;      // right: delete
                int y = x - k;
                while (x < N && y < M && A(x) == B(y)) { ++x; ++y; }
                v[static_cast<std::size_t>(off + k)] = x;
                if (x >= N && y >= M) { found_d = d; break; }
            }
        }
        if (found_d < 0) {
            // Wholesale rewrite past the D budget: not minimal, still exact.
            for (int x = 0; x < N; ++x) out.push_back({Edit::Del, pre + x, -1});
            for (int y = 0; y < M; ++y) out.push_back({Edit::Ins, -1, pre + y});
        } else {
            std::vector<Edit> rev;
            rev.reserve(static_cast<std::size_t>(N + M));
            int x = N, y = M;
            for (int d = found_d; d > 0; --d) {
                const auto& pv = trace[static_cast<std::size_t>(d)];
                const auto at = [&](int kk) { return pv[static_cast<std::size_t>(kk + d)]; };
                const int k = x - y;
                const int pk = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? k + 1 : k - 1;
                const int px = at(pk);
                const int py = px - pk;
                while (x > px && y > py) { --x; --y; rev.push_back({Edit::Keep, pre + x, pre + y}); }
                if (pk == k + 1) { --y; rev.push_back({Edit::Ins, -1, pre + y}); }
                else             { --x; rev.push_back({Edit::Del, pre + x, -1}); }
            }
            while (x > 0 && y > 0) { --x; --y; rev.push_back({Edit::Keep, pre + x, pre + y}); }
            // git order inside a change run: deletions before insertions.
            std::reverse(rev.begin(), rev.end());
            for (std::size_t i = 0; i < rev.size();) {
                if (rev[i].k == Edit::Keep) { out.push_back(rev[i++]); continue; }
                const std::size_t s = i;
                while (i < rev.size() && rev[i].k != Edit::Keep) ++i;
                for (std::size_t j = s; j < i; ++j) if (rev[j].k == Edit::Del) out.push_back(rev[j]);
                for (std::size_t j = s; j < i; ++j) if (rev[j].k == Edit::Ins) out.push_back(rev[j]);
            }
        }
    }
    for (int s = 0; ea + s < n; ++s) out.push_back({Edit::Keep, ea + s, eb + s});
    return out;
}

}  // namespace myers
