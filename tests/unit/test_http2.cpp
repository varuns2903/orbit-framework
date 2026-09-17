#include <gtest/gtest.h>
#include <orbit/http/Http2Session.hpp>
#include <orbit/http/HttpResponse.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace http;
using http::h2::detail::HeaderBlock;

namespace {

// Reads an nghttp2_nv back into owned strings so assertions do not depend on
// the borrowed pointers staying valid past the assertion itself.
std::pair<std::string, std::string> nv_at(const HeaderBlock& block, size_t i) {
    const nghttp2_nv& nv = block.nvs.at(i);
    return {
        std::string(reinterpret_cast<const char*>(nv.name), nv.namelen),
        std::string(reinterpret_cast<const char*>(nv.value), nv.valuelen)
    };
}

bool has_header(const HeaderBlock& block, std::string_view name) {
    for (size_t i = 0; i < block.nvs.size(); ++i) {
        if (nv_at(block, i).first == name) return true;
    }
    return false;
}

std::string value_of(const HeaderBlock& block, std::string_view name) {
    for (size_t i = 0; i < block.nvs.size(); ++i) {
        auto [k, v] = nv_at(block, i);
        if (k == name) return v;
    }
    return {};
}

} // namespace

// --- :method pseudo-header parsing ---

TEST(Http2MethodTest, ParsesEverySupportedMethod) {
    struct Case { const char* text; HttpMethod expected; };
    const Case cases[] = {
        {"GET", HttpMethod::GET},
        {"POST", HttpMethod::POST},
        {"PUT", HttpMethod::PUT},
        {"DELETE", HttpMethod::DELETE},
        {"PATCH", HttpMethod::PATCH},
        {"OPTIONS", HttpMethod::OPTIONS},
        {"HEAD", HttpMethod::HEAD},
    };

    for (const auto& c : cases) {
        HttpMethod out = HttpMethod::UNKNOWN;
        EXPECT_TRUE(h2::detail::parse_method(c.text, out)) << c.text;
        EXPECT_EQ(out, c.expected) << c.text;
    }
}

TEST(Http2MethodTest, RejectsUnknownMethodAndLeavesOutputUntouched) {
    HttpMethod out = HttpMethod::POST;
    EXPECT_FALSE(h2::detail::parse_method("BREW", out));
    EXPECT_EQ(out, HttpMethod::POST);
}

TEST(Http2MethodTest, MethodMatchingIsCaseSensitive) {
    // RFC 9110: method names are case-sensitive. "get" is not GET.
    HttpMethod out = HttpMethod::UNKNOWN;
    EXPECT_FALSE(h2::detail::parse_method("get", out));
    EXPECT_EQ(out, HttpMethod::UNKNOWN);
}

TEST(Http2MethodTest, RejectsEmptyMethod) {
    HttpMethod out = HttpMethod::UNKNOWN;
    EXPECT_FALSE(h2::detail::parse_method("", out));
}

// --- connection-specific header rejection (RFC 9113 section 8.2.2) ---

TEST(Http2HeaderFilterTest, RejectsConnectionSpecificHeaders) {
    EXPECT_TRUE(h2::detail::is_connection_specific_header("Connection"));
    EXPECT_TRUE(h2::detail::is_connection_specific_header("Transfer-Encoding"));
    EXPECT_TRUE(h2::detail::is_connection_specific_header("Keep-Alive"));
    EXPECT_TRUE(h2::detail::is_connection_specific_header("Proxy-Connection"));
    EXPECT_TRUE(h2::detail::is_connection_specific_header("Upgrade"));
}

TEST(Http2HeaderFilterTest, RejectionIsCaseInsensitive) {
    EXPECT_TRUE(h2::detail::is_connection_specific_header("connection"));
    EXPECT_TRUE(h2::detail::is_connection_specific_header("CONNECTION"));
    EXPECT_TRUE(h2::detail::is_connection_specific_header("TrAnSfEr-EnCoDiNg"));
}

TEST(Http2HeaderFilterTest, AllowsOrdinaryHeaders) {
    EXPECT_FALSE(h2::detail::is_connection_specific_header("Content-Type"));
    EXPECT_FALSE(h2::detail::is_connection_specific_header("X-Request-Id"));
    EXPECT_FALSE(h2::detail::is_connection_specific_header(""));
}

// --- response header block construction ---

TEST(Http2ResponseHeadersTest, StatusPseudoHeaderComesFirst) {
    HttpResponse res;
    res.status_code = HttpStatus::NotFound;

    HeaderBlock block = h2::detail::build_response_headers(res);

    ASSERT_FALSE(block.nvs.empty());
    auto [name, value] = nv_at(block, 0);
    EXPECT_EQ(name, ":status");
    EXPECT_EQ(value, "404");
}

TEST(Http2ResponseHeadersTest, StatusNameLengthMatchesTheName) {
    // A hand-written literal length here was previously easy to get wrong;
    // assert the declared length actually spans ":status".
    HttpResponse res;
    HeaderBlock block = h2::detail::build_response_headers(res);

    ASSERT_FALSE(block.nvs.empty());
    EXPECT_EQ(block.nvs[0].namelen, std::string(":status").size());
}

TEST(Http2ResponseHeadersTest, LowercasesHeaderNames) {
    HttpResponse res;
    res.headers["Content-Type"] = "application/json";
    res.headers["X-Custom-Header"] = "value";

    HeaderBlock block = h2::detail::build_response_headers(res);

    EXPECT_TRUE(has_header(block, "content-type"));
    EXPECT_TRUE(has_header(block, "x-custom-header"));
    EXPECT_FALSE(has_header(block, "Content-Type"));
}

TEST(Http2ResponseHeadersTest, PreservesHeaderValuesVerbatim) {
    HttpResponse res;
    res.headers["Content-Type"] = "Application/JSON; charset=UTF-8";

    HeaderBlock block = h2::detail::build_response_headers(res);

    // Only the name is lowercased; the value must survive untouched.
    EXPECT_EQ(value_of(block, "content-type"), "Application/JSON; charset=UTF-8");
}

TEST(Http2ResponseHeadersTest, DropsConnectionSpecificHeaders) {
    HttpResponse res;
    res.headers["Connection"] = "keep-alive";
    res.headers["Transfer-Encoding"] = "chunked";
    res.headers["Keep-Alive"] = "timeout=5";
    res.headers["Content-Type"] = "text/plain";

    HeaderBlock block = h2::detail::build_response_headers(res);

    EXPECT_FALSE(has_header(block, "connection"));
    EXPECT_FALSE(has_header(block, "transfer-encoding"));
    EXPECT_FALSE(has_header(block, "keep-alive"));
    EXPECT_TRUE(has_header(block, "content-type"));
    EXPECT_EQ(block.nvs.size(), 2u); // :status + content-type
}

TEST(Http2ResponseHeadersTest, NamePointersStayValidAfterConstruction) {
    // Regression test: the header list previously borrowed pointers into a
    // std::string that was scoped to the loop body building it, so every name
    // dangled by the time nghttp2 read the list. Reading the block back after
    // build_response_headers has returned reproduces that access pattern.
    HttpResponse res;
    res.status_code = HttpStatus::OK;
    for (int i = 0; i < 32; ++i) {
        res.headers["X-Header-" + std::to_string(i)] = "value-" + std::to_string(i);
    }

    HeaderBlock block = h2::detail::build_response_headers(res);

    ASSERT_EQ(block.nvs.size(), 33u);
    for (size_t i = 0; i < block.nvs.size(); ++i) {
        auto [name, value] = nv_at(block, i);
        EXPECT_FALSE(name.empty());
        EXPECT_EQ(name.size(), block.nvs[i].namelen);
        EXPECT_EQ(value.size(), block.nvs[i].valuelen);
        if (name != ":status") {
            EXPECT_EQ(name.rfind("x-header-", 0), 0u) << "corrupted name: " << name;
            EXPECT_EQ(value.rfind("value-", 0), 0u) << "corrupted value: " << value;
        }
    }
}

TEST(Http2ResponseHeadersTest, SurvivesBeingMovedAfterConstruction) {
    // The nv list borrows from block.storage, so moving the block must not
    // invalidate the pointers already handed out.
    HttpResponse res;
    res.headers["Content-Type"] = "text/html";

    HeaderBlock original = h2::detail::build_response_headers(res);
    HeaderBlock moved = std::move(original);

    EXPECT_EQ(value_of(moved, "content-type"), "text/html");
    EXPECT_EQ(value_of(moved, ":status"), "200");
}

TEST(Http2ResponseHeadersTest, HandlesResponseWithNoHeaders) {
    HttpResponse res;
    res.status_code = HttpStatus::NoContent;

    HeaderBlock block = h2::detail::build_response_headers(res);

    ASSERT_EQ(block.nvs.size(), 1u);
    EXPECT_EQ(value_of(block, ":status"), "204");
}

TEST(Http2ResponseHeadersTest, EncodesServerErrorStatus) {
    HttpResponse res;
    res.status_code = HttpStatus::InternalServerError;

    HeaderBlock block = h2::detail::build_response_headers(res);

    EXPECT_EQ(value_of(block, ":status"), "500");
}
