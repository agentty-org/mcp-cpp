// SPDX-License-Identifier: Apache-2.0
//
// diff.cpp — unified-diff engine over Myers' algorithm (util/myers.hpp, the
// same one agentty's src/diff/diff.cpp uses). compute + render_unified only;
// apply_accepted is a host-UI concern and stays in the host.

#include "diff.hpp"

#include <mcp/tools/util/myers.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace mcp::tools::detail::diff {

namespace {
using Edit = myers::Edit;
using myers::detail::split_views;

// Myers' O((N+M)D) diff (util/myers.hpp, shared with the host): cost follows
// how much changed, not how big the file is. The LCS table it replaced was
// O(N*M) and page-faulted through megabytes for a one-line edit.
std::vector<Edit> compute_edits(const std::vector<std::string_view>& a,
                                const std::vector<std::string_view>& b) {
    return myers::diff(a, b);
}
} // namespace

Diff compute(const std::string& path,
             const std::string& before,
             const std::string& after) {
    Diff c;
    c.path   = path;

    // Only the changed region (plus context) is split into lines; `off` maps
    // window line numbers back to file line numbers.
    const auto win = myers::window(before, after, 3);
    const auto& a = win.a;
    const auto& b = win.b;
    const int off = win.off;
    auto edits = compute_edits(a, b);

    const int ctx = 3;
    int added = 0, removed = 0;
    std::vector<bool> is_change(edits.size(), false);
    for (size_t k = 0; k < edits.size(); ++k)
        if (edits[k].k != Edit::Keep) is_change[k] = true;

    size_t k = 0;
    while (k < edits.size()) {
        while (k < edits.size() && !is_change[k]) ++k;
        if (k >= edits.size()) break;
        size_t start = (k > (size_t)ctx) ? k - ctx : 0;
        size_t end = k;
        while (end < edits.size()) {
            size_t last_change = end;
            size_t probe = end;
            size_t gap = 0;
            while (probe < edits.size() && gap <= (size_t)(2 * ctx)) {
                if (is_change[probe]) { last_change = probe; gap = 0; }
                else gap++;
                probe++;
            }
            if (last_change == end) break;
            end = last_change;
        }
        end = std::min(edits.size() - 1, end + ctx);

        Hunk h;
        int old_start = -1, new_start = -1;
        int old_len = 0, new_len = 0;
        std::ostringstream patch;
        // Emit deletions before insertions within each contiguous change run
        // (git convention). The LCS backtrace can group inserts ahead of
        // deletes; rendered through a two-column diff gutter that ordering
        // makes the old/new line numbers read out of sequence (new line 2
        // appearing above old line 2). Buffer each run and flush "-" before
        // "+" on the next context line so both gutter columns stay monotonic
        // and the changed lines line up.
        std::string del_buf, ins_buf;
        auto flush_run = [&] {
            if (!del_buf.empty()) patch << del_buf;
            if (!ins_buf.empty()) patch << ins_buf;
            del_buf.clear();
            ins_buf.clear();
        };
        for (size_t i2 = start; i2 <= end; ++i2) {
            const auto& e = edits[i2];
            if (e.k == Edit::Keep) {
                flush_run();
                if (old_start < 0) old_start = off + e.a_idx + 1;
                if (new_start < 0) new_start = off + e.b_idx + 1;
                old_len++; new_len++;
                patch << " " << a[static_cast<std::size_t>(e.a_idx)] << "\n";
            } else if (e.k == Edit::Del) {
                if (old_start < 0) old_start = off + e.a_idx + 1;
                old_len++;
                del_buf += "-"; del_buf += a[static_cast<std::size_t>(e.a_idx)]; del_buf += "\n";
                removed++;
            } else {
                if (new_start < 0) new_start = off + e.b_idx + 1;
                new_len++;
                ins_buf += "+"; ins_buf += b[static_cast<std::size_t>(e.b_idx)]; ins_buf += "\n";
                added++;
            }
        }
        flush_run();
        h.old_start = std::max(1, old_start);
        h.new_start = std::max(1, new_start);
        h.old_len = old_len;
        h.new_len = new_len;
        h.patch = patch.str();
        c.hunks.push_back(std::move(h));
        k = end + 1;
    }

    c.added = added;
    c.removed = removed;
    return c;
}

std::string render_unified(const Diff& c) {
    std::ostringstream oss;
    oss << "--- a/" << c.path << "\n";
    oss << "+++ b/" << c.path << "\n";
    for (const auto& h : c.hunks) {
        oss << "@@ -" << h.old_start << "," << h.old_len
            << " +" << h.new_start << "," << h.new_len << " @@\n";
        oss << h.patch;
    }
    return oss.str();
}

} // namespace mcp::tools::detail::diff
