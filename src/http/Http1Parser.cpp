#include <orbit/http/Http1Parser.hpp>
#include <orbit/http/HttpParser.hpp>

#include <llhttp.h>

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace http {

struct Http1Parser::Impl {
    explicit Impl(Limits l) : limits(l) {
        llhttp_settings_init(&settings);
        settings.on_method = on_method;
        settings.on_url = on_url;
        settings.on_header_field = on_header_field;
        settings.on_header_field_complete = on_header_field_complete;
        settings.on_header_value = on_header_value;
        settings.on_header_value_complete = on_header_value_complete;
        settings.on_headers_complete = on_headers_complete;
        settings.on_body = on_body;
        settings.on_message_complete = on_message_complete;
        llhttp_init(&parser, HTTP_REQUEST, &settings);
        parser.data = this;
    }

    Limits limits;
    llhttp_t parser{};
    llhttp_settings_t settings{};

    HttpRequest request;
    std::string body;
    std::function<void(std::string_view)> body_handler;
    std::function<size_t(const HttpRequest&)> body_limit_for;
    size_t body_limit = 0; // for the current message

    // The message being parsed.
    std::string method;
    std::string url;
    std::string field;
    std::string value;
    std::vector<std::pair<std::string, std::string>> headers;
    size_t header_bytes = 0;
    size_t body_bytes = 0;
    bool headers_done = false;
    bool message_done = false;
    bool expect_continue = false;
    bool chunked = false;
    bool has_body = false;
    bool upgrade = false;

    bool failed = false;
    int error_status = 0;
    std::string error_reason;

    static Impl& of(llhttp_t* p) { return *static_cast<Impl*>(p->data); }

    // Records an error found by a callback; the callback then returns -1.
    int fail(int status, std::string reason) {
        if (!failed) {
            failed = true;
            error_status = status;
            error_reason = std::move(reason);
        }
        return -1;
    }

    void reset_message() {
        request = HttpRequest{};
        body.clear();
        body_handler = nullptr;
        method.clear();
        url.clear();
        field.clear();
        value.clear();
        headers.clear();
        header_bytes = 0;
        body_bytes = 0;
        headers_done = false;
        message_done = false;
        expect_continue = false;
        chunked = false;
        has_body = false;
        upgrade = false;
    }

    static int on_method(llhttp_t* p, const char* at, size_t len) {
        Impl& self = of(p);
        self.method.append(at, len);
        return 0;
    }

    static int on_url(llhttp_t* p, const char* at, size_t len) {
        Impl& self = of(p);
        self.url.append(at, len);
        // Checked as it arrives, so an endless request line fails before
        // its CRLF (431, as for an oversized header section).
        if (self.method.size() + 1 + self.url.size() > self.limits.max_request_line) {
            return self.fail(431, "Request line too long");
        }
        return 0;
    }

    static int on_header_field(llhttp_t* p, const char* at, size_t len) {
        of(p).field.append(at, len);
        return 0;
    }

    static int on_header_field_complete(llhttp_t* p) {
        Impl& self = of(p);
        if (self.headers.size() + 1 > self.limits.max_headers) {
            return self.fail(431, "Too many header fields");
        }
        return 0;
    }

    static int on_header_value(llhttp_t* p, const char* at, size_t len) {
        of(p).value.append(at, len);
        return 0;
    }

    static int on_header_value_complete(llhttp_t* p) {
        Impl& self = of(p);
        // Optional whitespace around the value is not part of it (RFC 9110
        // section 5.5); llhttp drops the leading part.
        while (!self.value.empty() && (self.value.back() == ' ' || self.value.back() == '\t')) {
            self.value.pop_back();
        }
        self.headers.emplace_back(std::move(self.field), std::move(self.value));
        self.field.clear();
        self.value.clear();
        return 0;
    }

    static bool iequals(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }

    static int on_headers_complete(llhttp_t* p) {
        Impl& self = of(p);
        self.headers_done = true;

        HttpRequest& req = self.request;
        req.method = HttpParser::parse_method(self.method);
        req.target = self.url;
        const size_t q = self.url.find('?');
        const std::string_view path = std::string_view(self.url).substr(0, q);
        // Routes, params and static files all see the decoded path.
        if (!percent_decode(path, req.uri, false, true)) {
            return self.fail(400, "Invalid percent-encoding in the path");
        }
        if (q != std::string::npos && !parse_urlencoded(std::string_view(self.url).substr(q + 1), req.query)) {
            return self.fail(400, "Invalid query string");
        }
        req.http_version = "HTTP/" + std::to_string(p->http_major) + "." + std::to_string(p->http_minor);

        for (auto& [name, val] : self.headers) {
            if (iequals(name, "Expect") && iequals(val, "100-continue")) self.expect_continue = true;
            req.set_header(name, std::move(val)); // a repeated field: the last one wins
        }
        auto cookie = req.headers.find("Cookie");
        if (cookie != req.headers.end()) parse_cookie_header(cookie->second, req.cookies);

        self.body_limit = self.body_limit_for ? self.body_limit_for(req) : self.limits.max_body_size;
        self.chunked = (p->flags & F_CHUNKED) != 0;
        const bool has_length = (p->flags & F_CONTENT_LENGTH) != 0;
        self.has_body = self.chunked || (has_length && p->content_length > 0);
        // Refuse an oversized body from its declared length, before reading it.
        if (has_length && p->content_length > self.body_limit) {
            return self.fail(413, "Content-Length exceeds the body size limit");
        }
        // Pause only when a body follows: the caller may want to stream it or
        // send "100 Continue" first. Without a body the message is complete
        // here, and pausing would leave it waiting for bytes that never come.
        return self.has_body ? HPE_PAUSED : 0;
    }

    static int on_body(llhttp_t* p, const char* at, size_t len) {
        Impl& self = of(p);
        self.body_bytes += len;
        if (self.body_bytes > self.body_limit) {
            return self.fail(413, "Body exceeds the size limit");
        }
        if (self.body_handler) {
            self.body_handler(std::string_view(at, len));
        } else {
            self.body.append(at, len);
        }
        return 0;
    }

    static int on_message_complete(llhttp_t* p) {
        Impl& self = of(p);
        self.message_done = true;
        self.upgrade = p->upgrade != 0;
        self.request.body = self.body;
        return HPE_PAUSED;
    }

    // An llhttp error that no callback explained.
    void fail_from_llhttp(llhttp_errno_t err) {
        const char* reason = llhttp_get_error_reason(&parser);
        // A method token llhttp does not know at all (RFC 9110 section 15.6.2).
        fail(err == HPE_INVALID_METHOD ? 501 : 400, reason ? reason : llhttp_errno_name(err));
    }
};

Http1Parser::Http1Parser(Limits limits) : impl_(std::make_unique<Impl>(limits)) {}
Http1Parser::~Http1Parser() = default;

Http1Parser::Event Http1Parser::feed(std::string_view data, size_t& consumed) {
    Impl& s = *impl_;
    consumed = 0;
    if (s.failed) return Event::Error;
    if (s.message_done) return Event::MessageComplete; // next() first

    llhttp_errno_t err = llhttp_get_errno(&s.parser);
    if (err == HPE_PAUSED) llhttp_resume(&s.parser);

    const bool in_headers = !s.headers_done;
    err = llhttp_execute(&s.parser, data.data(), data.size());
    if (err == HPE_PAUSED_UPGRADE && !s.message_done) {
        // Left over from an upgrade request the caller declined (it called
        // next() instead of switching protocols): llhttp still owes that
        // pause and stops here without consuming anything. Clear it and
        // parse these bytes as HTTP. Reporting it as a pause instead made
        // every caller loop forever on zero progress.
        llhttp_resume_after_upgrade(&s.parser);
        err = llhttp_execute(&s.parser, data.data(), data.size());
    }

    size_t used = data.size();
    if (err == HPE_PAUSED || err == HPE_PAUSED_UPGRADE) {
        used = static_cast<size_t>(llhttp_get_error_pos(&s.parser) - data.data());
    }
    consumed = used;

    if (in_headers && !s.failed) {
        // Counted from the bytes fed, so an endless header section fails
        // before its blank line arrives.
        s.header_bytes += (err == HPE_OK || err == HPE_PAUSED || err == HPE_PAUSED_UPGRADE) ? used : 0;
        if (s.header_bytes > s.limits.max_header_bytes) {
            s.fail(431, "Header section too large");
        }
    }
    if (s.failed) return Event::Error;

    switch (err) {
        case HPE_OK:
            return Event::NeedMore;
        case HPE_PAUSED:
        case HPE_PAUSED_UPGRADE:
            return s.message_done ? Event::MessageComplete : Event::HeadersComplete;
        default:
            s.fail_from_llhttp(err);
            return Event::Error;
    }
}

void Http1Parser::next() {
    Impl& s = *impl_;
    if (s.failed) return;
    llhttp_errno_t err = llhttp_get_errno(&s.parser);
    if (err == HPE_PAUSED) {
        llhttp_resume(&s.parser);
    } else if (err == HPE_PAUSED_UPGRADE) {
        // The caller did not switch protocols; keep reading HTTP.
        llhttp_resume_after_upgrade(&s.parser);
    }
    s.reset_message();
}

HttpRequest& Http1Parser::request() { return impl_->request; }

void Http1Parser::set_body_handler(std::function<void(std::string_view)> handler) {
    impl_->body_handler = std::move(handler);
}

void Http1Parser::set_body_limit(std::function<size_t(const HttpRequest&)> limit_for) {
    impl_->body_limit_for = std::move(limit_for);
}

bool Http1Parser::expect_continue() const { return impl_->expect_continue; }
bool Http1Parser::chunked() const { return impl_->chunked; }
bool Http1Parser::has_body() const { return impl_->has_body; }
bool Http1Parser::keep_alive() const { return llhttp_should_keep_alive(&impl_->parser) != 0; }
bool Http1Parser::upgrade() const { return impl_->upgrade; }
int Http1Parser::error_status() const { return impl_->error_status; }
const std::string& Http1Parser::error_reason() const { return impl_->error_reason; }

} // namespace http
