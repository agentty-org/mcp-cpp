// SPDX-License-Identifier: Apache-2.0
#include <mcp/tools/util/glob.hpp>

#include <string>
#include <vector>

namespace mcp::tools::util {

bool glob_match(std::string_view pattern, std::string_view name) noexcept {
    size_t pi = 0, ni = 0, star = std::string_view::npos, match = 0;
#ifdef _WIN32
    auto eq = [](char a, char b) {
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        return a == b;
    };
#else
    auto eq = [](char a, char b) { return a == b; };
#endif
    while (ni < name.size()) {
        if (pi < pattern.size() && pattern[pi] == '*') {
            star = pi++;
            match = ni;
            while (pi < pattern.size() && pattern[pi] == '*') pi++;  // collapse **
        } else if (pi < pattern.size() && pattern[pi] == '?') {
            pi++; ni++;
        } else if (pi < pattern.size() && pattern[pi] == '[') {
            size_t close = pattern.find(']', pi + 1);
            if (close == std::string_view::npos) {
                if (eq(pattern[pi], name[ni])) { pi++; ni++; }
                else if (star != std::string_view::npos) { pi = star + 1; ni = ++match; }
                else return false;
                continue;
            }
            bool neg = pi + 1 < close && pattern[pi + 1] == '!';
            bool hit = false;
            size_t j = pi + 1 + (neg ? 1 : 0);
            while (j < close) {
                if (j + 2 < close && pattern[j + 1] == '-') {
                    if (name[ni] >= pattern[j] && name[ni] <= pattern[j + 2]) hit = true;
                    j += 3;
                } else {
                    if (eq(pattern[j], name[ni])) hit = true;
                    j++;
                }
            }
            if (hit != neg) { pi = close + 1; ni++; }
            else if (star != std::string_view::npos) { pi = star + 1; ni = ++match; }
            else return false;
        } else if (pi < pattern.size() && eq(pattern[pi], name[ni])) {
            pi++; ni++;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            ni = ++match;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') pi++;
    return pi == pattern.size();
}

namespace {

// `{a,b}` -> {"a","b"}, expanded left to right, nested braces included.
// An unbalanced '{' is a literal. Bounded so a hostile pattern can't blow up.
void expand_braces(std::string_view p, std::vector<std::string>& out) {
    constexpr std::size_t kMaxAlternatives = 256;
    if (out.size() >= kMaxAlternatives) return;
    std::size_t open = std::string_view::npos;
    int depth = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '\\') { ++i; continue; }
        if (p[i] == '{') { if (depth++ == 0) open = i; }
        else if (p[i] == '}' && depth > 0 && --depth == 0) {
            const auto head = p.substr(0, open);
            const auto body = p.substr(open + 1, i - open - 1);
            const auto tail = p.substr(i + 1);
            std::size_t start = 0;
            int d = 0;
            for (std::size_t k = 0; k <= body.size(); ++k) {
                if (k < body.size()) {
                    if (body[k] == '{') ++d;
                    else if (body[k] == '}') --d;
                    if (!(body[k] == ',' && d == 0)) continue;
                }
                std::string alt{head};
                alt += body.substr(start, k - start);
                alt += tail;
                expand_braces(alt, out);
                start = k + 1;
            }
            return;
        }
    }
    out.emplace_back(p);
}

[[nodiscard]] std::vector<std::string_view> split_segments(std::string_view s) {
    std::vector<std::string_view> v;
    std::size_t i = 0;
    while (i <= s.size()) {
        const auto j = s.find('/', i);
        const auto seg = s.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i);
        if (!seg.empty() && seg != ".") v.push_back(seg);
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return v;
}

// Segment matcher with `**` spanning whole segments. Iterative backtracking
// over the single most recent `**`, which is exact for globs (a later `**`
// subsumes an earlier one's choice) and linear-ish in practice.
[[nodiscard]] bool match_segments(const std::vector<std::string_view>& pat,
                                  const std::vector<std::string_view>& path) {
    std::size_t pi = 0, si = 0;
    std::size_t star = std::string_view::npos, mark = 0;
    while (si < path.size()) {
        if (pi < pat.size() && pat[pi] == "**") {
            star = pi++;
            mark = si;
        } else if (pi < pat.size() && glob_match(pat[pi], path[si])) {
            ++pi; ++si;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == "**") ++pi;
    return pi == pat.size();
}

[[nodiscard]] bool match_one(std::string_view pat, std::string_view rel) {
    while (pat.starts_with("./")) pat.remove_prefix(2);
    while (pat.starts_with('/')) pat.remove_prefix(1);
    if (pat.find('/') == std::string_view::npos && pat != "**") {
        const auto slash = rel.rfind('/');
        return glob_match(pat, slash == std::string_view::npos ? rel : rel.substr(slash + 1));
    }
    return match_segments(split_segments(pat), split_segments(rel));
}

}  // namespace

bool glob_path_match(std::string_view pattern, std::string_view rel_path) {
    if (pattern.empty()) return true;
    if (pattern.find('{') == std::string_view::npos) return match_one(pattern, rel_path);
    std::vector<std::string> alts;
    expand_braces(pattern, alts);
    for (const auto& a : alts)
        if (match_one(a, rel_path)) return true;
    return false;
}

bool glob_path_match(std::string_view pattern, const std::filesystem::path& file,
                     const std::filesystem::path& root) {
    if (pattern.empty()) return true;
    // Lexical: callers pass a path they walked from `root`.
    const auto rel = file.lexically_relative(root);
    std::string rp = (rel.empty() || *rel.begin() == ".." ? file : rel).generic_string();
    return glob_path_match(pattern, rp);
}

} // namespace mcp::tools::util
