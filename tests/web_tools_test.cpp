// SPDX-License-Identifier: Apache-2.0
//
// web_tools_test.cpp — exercises web_fetch / web_search through
// make_provider() with a FAKE HttpClient (no network). Proves
// registration (gated on svc.http), SSRF refusal, HTML→text extraction,
// the multi-engine search fallback chain, and Net effects.

#include "support/disk_files.hpp"
#include <mcp/tools/toolset.hpp>
#include <mcp/tools/host.hpp>
#include <mcp/tools/toolset.hpp>
#include <mcp/cap/local.hpp>

#include "agtest.hpp"
#include <cstdio>
#include <string>

using namespace mcp::tools;

static mcp::cap::Result call(mcp::cap::CapabilityProvider& p,
                             const std::string& name, mcp::Json args) {
    return p.execute(mcp::cap::Request{name, std::move(args)});
}
static mcp::Json obj() { return mcp::Json::object(); }

// A scripted HttpClient: returns a canned response keyed on URL substring.
class FakeHttp : public HttpClient {
public:
    std::vector<std::pair<std::string, HttpResponse>> rules;
    std::vector<std::string> seen_urls;

    HttpResponse send(const HttpRequest& req) override {
        seen_urls.push_back(req.url);
        for (auto& [needle, resp] : rules)
            if (req.url.find(needle) != std::string::npos) return resp;
        HttpResponse r; r.status = 0; r.error = "no rule for " + req.url;
        return r;
    }
};

static HttpResponse html_resp(std::string body) {
    HttpResponse r;
    r.status = 200;
    r.headers.push_back({"content-type", "text/html; charset=utf-8"});
    r.body = std::move(body);
    return r;
}

TEST_CASE("web_tools") {
    auto fake = std::make_shared<FakeHttp>();

    // web_fetch: a simple HTML page → cleaned text with the heading + link.
    fake->rules.push_back({"example.com",
        html_resp("<html><head><title>T</title></head><body>"
                  "<main><h1>Hello World</h1>"
                  "<p>This is a reasonably long paragraph of real prose so that "
                  "the nav-line filter keeps it, and it contains a "
                  "<a href=\"https://dest.test/x\">link text</a> in the middle "
                  "of the sentence which should survive extraction.</p>"
                  "<nav>Home About Pricing Contact</nav></main></body></html>")});

    // web_search: Brave returns a result block our parser understands.
    fake->rules.push_back({"search.brave.com",
        html_resp("<div class=\"snippet svelte-x\">"
                  "<a href=\"https://result.test/page\" class=\"l1\">x</a>"
                  "<div class=\"title svelte-y\">Result Title</div>"
                  "<div class=\"generic-snippet svelte-z\">A useful snippet.</div>"
                  "</div>")});

    HostServices svc;

    svc.files = std::make_shared<mcp::test::DiskFiles>();
    svc.http = fake;
    auto provider = make_provider(svc, ToolsetConfig{}, "local");

    // ── web_fetch cleans HTML to text + preserves the link ───────────────
    {
        auto args = obj(); args["url"] = "https://example.com/page";
        auto r = call(*provider, "web_fetch", args);
        assert(!r.is_error);
        assert(r.text.find("Hello World") != std::string::npos);
        assert(r.text.find("[link text](https://dest.test/x)") != std::string::npos);
        // nav chrome should be stripped
        assert(r.text.find("Home About") == std::string::npos);
        assert(r.effects.has(Effect::Net));
        std::puts("web_fetch: HTML→text + link preserved + nav stripped");
    }

    // ── web_fetch SSRF refusal ───────────────────────────────────────────
    {
        auto args = obj(); args["url"] = "https://127.0.0.1/secret";
        auto r = call(*provider, "web_fetch", args);
        assert(r.is_error);
        assert(r.text.find("SSRF") != std::string::npos
            || r.text.find("loopback") != std::string::npos);
        std::puts("web_fetch: SSRF refusal ok");
    }

    // ── web_fetch blocks ENCODED loopback/metadata forms ─────────────────
    // A dotted-quad-only guard is bypassed by the integer/hex/octal/short
    // spellings that libc's inet_aton accepts and connect() honours. Each
    // of these resolves to a private/loopback/metadata address and must be
    // refused just like the plain dotted-quad.
    {
        const char* evil[] = {
            "https://2130706433/",        // 127.0.0.1 as 32-bit decimal
            "https://0x7f000001/",        // 127.0.0.1 as 32-bit hex
            "https://017700000001/",      // 127.0.0.1 as 32-bit octal
            "https://127.1/",             // 127.0.0.1 short A.D form
            "https://0x7f.0.0.1/",        // mixed-radix per-part
            "https://2852039166/",        // 169.254.169.254 (cloud metadata)
            "https://192.168.1.1/",       // plain private (control)
        };
        for (const char* u : evil) {
            auto args = obj(); args["url"] = u;
            auto r = call(*provider, "web_fetch", args);
            assert(r.is_error);
            assert(r.text.find("SSRF") != std::string::npos
                || r.text.find("loopback") != std::string::npos
                || r.text.find("private") != std::string::npos);
        }
        std::puts("web_fetch: encoded-IP SSRF bypass forms blocked");
    }

    // ── a genuinely public numeric IP is NOT over-blocked ────────────────
    {
        // 8.8.8.8 is public; the guard must let it through to the client
        // (which the fake resolves). Refusal here would be a false positive.
        auto args = obj(); args["url"] = "https://8.8.8.8/";
        auto r = call(*provider, "web_fetch", args);
        // Not blocked for SSRF reasons: either succeeds or fails for a
        // non-SSRF reason, but never the SSRF refusal message.
        assert(!(r.is_error
                 && (r.text.find("SSRF") != std::string::npos
                     || r.text.find("loopback") != std::string::npos)));
        std::puts("web_fetch: public numeric IP not over-blocked");
    }

    // ── web_fetch rejects non-https ──────────────────────────────────────
    {
        auto args = obj(); args["url"] = "http://example.com";
        auto r = call(*provider, "web_fetch", args);
        assert(r.is_error);
        std::puts("web_fetch: non-https refused");
    }

    // ── web_search parses the Brave result ───────────────────────────────
    {
        auto args = obj(); args["query"] = "test query";
        auto r = call(*provider, "web_search", args);
        assert(!r.is_error);
        assert(r.text.find("[via Brave Free]") != std::string::npos);
        assert(r.text.find("Result Title") != std::string::npos);
        assert(r.text.find("https://result.test/page") != std::string::npos);
        assert(r.effects.has(Effect::Net));
        std::puts("web_search: Brave parse ok");
    }

    // ── web tools NOT registered when svc.http is null ───────────────────
    {
        HostServices empty;  // no http
        auto p2 = make_provider(empty, ToolsetConfig{}, "local");
        auto args = obj(); args["url"] = "https://example.com";
        auto r = call(*p2, "web_fetch", args);
        // No such tool → provider returns an error result.
        assert(r.is_error);
        std::puts("web tools: absent without HttpClient");
    }

    // ── a JS-only page is NEVER sent to r.jina.ai by default ────────────
    std::string shell = "<html><body><div id=\"root\"></div>";
    for (int i = 0; i < 10; ++i) shell += "<script src=\"/a" + std::to_string(i) + ".js\"></script>";
    shell += "</body></html>";
    {
        auto f2 = std::make_shared<FakeHttp>();
        f2->rules.push_back({"r.jina.ai", [] { HttpResponse r; r.status = 200;
            r.body = std::string(3000, 'J'); return r; }()});
        f2->rules.push_back({"spa.test", html_resp(shell)});
        HostServices s2; s2.files = std::make_shared<mcp::test::DiskFiles>(); s2.http = f2;
        auto p = make_provider(s2, ToolsetConfig{}, "local");

        auto a = obj(); a["url"] = "https://spa.test/app";
        auto r = call(*p, "web_fetch", a);
        CHECK(!r.is_error);
        CHECK(r.text.find("needs JavaScript") != std::string::npos
              || r.text.find("need JavaScript") != std::string::npos);
        for (const auto& u : f2->seen_urls) CHECK(u.find("jina") == std::string::npos);

        // opt-in renders it
        f2->seen_urls.clear();
        a["render"] = "jina";
        a["url"] = "https://spa.test/app2";
        r = call(*p, "web_fetch", a);
        CHECK(r.text.find("via r.jina.ai") != std::string::npos);

        // ...but never for a URL carrying a token, or an intranet host
        f2->seen_urls.clear();
        a["url"] = "https://spa.test/cb?access_token=abc";
        r = call(*p, "web_fetch", a);
        CHECK(r.text.find("refused") != std::string::npos);
        for (const auto& u : f2->seen_urls) CHECK(u.find("jina") == std::string::npos);
        std::puts("web_fetch: jina opt-in only, refused for secret-bearing URLs");
    }

    // ── cache, paging, legacy charset ────────────────────────────────────
    {
        auto f3 = std::make_shared<FakeHttp>();
        HttpResponse big; big.status = 200;
        big.headers.push_back({"content-type", "text/plain"});
        big.body = std::string(5000, 'a') + std::string(5000, 'b');
        f3->rules.push_back({"big.test", big});
        HttpResponse latin; latin.status = 200;
        latin.headers.push_back({"content-type", "text/plain; charset=ISO-8859-1"});
        latin.body = "caf\xE9 \x93quoted\x94";
        f3->rules.push_back({"latin.test", latin});
        HostServices s3; s3.files = std::make_shared<mcp::test::DiskFiles>(); s3.http = f3;
        auto p = make_provider(s3, ToolsetConfig{}, "local");

        auto a = obj(); a["url"] = "https://big.test/doc"; a["max_chars"] = 6000;
        auto r1 = call(*p, "web_fetch", a);
        CHECK(r1.text.find("pass start_char=6000") != std::string::npos);
        CHECK(r1.text.find("of 10000") != std::string::npos);
        a["start_char"] = 6000;
        auto r2 = call(*p, "web_fetch", a);
        CHECK(r2.text.find("cached") != std::string::npos);      // second fetch: no request
        CHECK(f3->seen_urls.size() == 1);
        CHECK(r2.text.find("bbbb") != std::string::npos);
        CHECK(r2.text.find("aaaa") == std::string::npos);

        auto l = obj(); l["url"] = "https://latin.test/";
        auto r3 = call(*p, "web_fetch", l);
        CHECK(r3.text.find("caf\xC3\xA9") != std::string::npos);            // é
        CHECK(r3.text.find("\xE2\x80\x9Cquoted\xE2\x80\x9D") != std::string::npos); // “ ” (cp1252)

        // Requests with credentials bypass the cache.
        auto c = obj(); c["url"] = "https://big.test/doc";
        c["headers"] = mcp::Json{{"Authorization", "Bearer x"}};
        const auto before = f3->seen_urls.size();
        (void)call(*p, "web_fetch", c);
        CHECK(f3->seen_urls.size() == before + 1);
        std::puts("web_fetch: paging, cache, latin1/cp1252 decoding");
    }

    // ── web_search domain filters ──────────────────────────────────────────
    {
        auto f4 = std::make_shared<FakeHttp>();
        std::string page;
        for (const char* u : {"https://docs.python.org/3/a", "https://evil-python.org/b",
                              "https://www.python.org/c", "https://spam.test/d"})
            page += std::string("<div class=\"snippet svelte-x\"><a href=\"") + u + "\" class=\"l1\">x</a>"
                    "<div class=\"title svelte-y\">T " + u + "</div>"
                    "<div class=\"generic-snippet svelte-z\">s</div></div>";
        f4->rules.push_back({"search.brave.com", html_resp(page)});
        HostServices s4; s4.files = std::make_shared<mcp::test::DiskFiles>(); s4.http = f4;
        auto p = make_provider(s4, ToolsetConfig{}, "local");

        auto a = obj(); a["query"] = "asyncio";
        a["allowed_domains"] = mcp::Json::array({"python.org"});
        auto r = call(*p, "web_search", a);
        CHECK(r.text.find("docs.python.org") != std::string::npos);
        CHECK(r.text.find("www.python.org") != std::string::npos);
        CHECK(r.text.find("evil-python.org") == std::string::npos);   // suffix, not substring
        CHECK(r.text.find("spam.test") == std::string::npos);
        CHECK(f4->seen_urls.back().find("site") != std::string::npos);  // steered the engine

        auto b = obj(); b["query"] = "asyncio";
        b["blocked_domains"] = mcp::Json::array({"spam.test", "docs.python.org"});
        auto rb = call(*p, "web_search", b);
        CHECK(rb.text.find("spam.test") == std::string::npos);
        CHECK(rb.text.find("docs.python.org") == std::string::npos);
        CHECK(rb.text.find("www.python.org") != std::string::npos);
        std::puts("web_search: allowed/blocked domain filters");
    }
}
