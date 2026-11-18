// SPDX-License-Identifier: Apache-2.0
#pragma once
// find_bytes — substring search that stays fast when the needle's first byte
// is common.
//
// libstdc++'s string::find(const char*) is memchr-on-the-first-byte, then
// memcmp. In source code the first byte of a needle is almost always common
// (a space of indentation, `a` of `auto`, `r` of `return`), so it stops every
// few bytes: 75% of a 20-edit batch on a 1 MiB file went there. glibc's
// memmem is two-way with a SIMD skip loop and does not have that cliff.
// Windows and other libcs get std::boyer_moore_horspool_searcher.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string_view>

#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <string.h>
#define MCP_HAVE_MEMMEM 1
#endif

namespace mcp::tools::util {

// Position of the first `needle` in `hay` at or after `from`, or npos.
[[nodiscard]] inline std::size_t find_bytes(std::string_view hay, std::string_view needle,
                                            std::size_t from = 0) noexcept {
    if (from > hay.size()) return std::string_view::npos;
    if (needle.empty()) return from;
    if (needle.size() > hay.size() - from) return std::string_view::npos;
    if (needle.size() == 1) return hay.find(needle[0], from);   // memchr
#if defined(MCP_HAVE_MEMMEM)
    const void* p = ::memmem(hay.data() + from, hay.size() - from, needle.data(), needle.size());
    return p ? static_cast<std::size_t>(static_cast<const char*>(p) - hay.data())
             : std::string_view::npos;
#else
    if (needle.size() < 4) return hay.find(needle, from);
    const auto it = std::search(hay.begin() + static_cast<std::ptrdiff_t>(from), hay.end(),
                                std::boyer_moore_horspool_searcher(needle.begin(), needle.end()));
    return it == hay.end() ? std::string_view::npos
                           : static_cast<std::size_t>(it - hay.begin());
#endif
}

// Non-overlapping occurrences of `needle`, stopping once `cap` are found
// (callers asking "is it unique?" pass 2).
[[nodiscard]] inline int count_bytes(std::string_view hay, std::string_view needle,
                                     int cap = 1 << 30) noexcept {
    if (needle.empty()) return 0;
    int n = 0;
    for (std::size_t p = find_bytes(hay, needle); p != std::string_view::npos && n < cap;
         p = find_bytes(hay, needle, p + needle.size()))
        ++n;
    return n;
}

}  // namespace mcp::tools::util
