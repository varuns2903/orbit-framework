#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/http/MultipartUpload.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

// Request bodies over HTTP/1.1: Expect: 100-continue, form fields and
// multipart uploads streamed to disk.

namespace {

constexpr uint16_t kPort = 8141;
constexpr size_t kMaxBody = 64 * 1024;

network::socket_t connect_to(int timeout_ms = 3000) {
    network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        network::close_socket(fd);
        return network::INVALID_SOCKET_FD;
    }
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(timeout_ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return fd;
}

void send_str(network::socket_t fd, const std::string& s) {
    size_t sent = 0;
    while (sent < s.size()) {
        auto n = ::send(fd, s.data() + sent, static_cast<int>(s.size() - sent), 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

std::string read_some(network::socket_t fd) {
    char buf[8192];
    auto n = ::recv(fd, buf, sizeof(buf), 0);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

// Reads one response: headers plus a Content-Length body.
std::string read_response(network::socket_t fd) {
    std::string res;
    while (res.find("\r\n\r\n") == std::string::npos) {
        std::string chunk = read_some(fd);
        if (chunk.empty()) return res;
        res += chunk;
    }
    size_t cl = res.find("Content-Length: ");
    if (cl != std::string::npos) {
        size_t len = std::stoul(res.substr(cl + 16));
        size_t want = res.find("\r\n\r\n") + 4 + len;
        while (res.size() < want) {
            std::string chunk = read_some(fd);
            if (chunk.empty()) break;
            res += chunk;
        }
    }
    return res;
}

std::string body_of(const std::string& response) {
    size_t p = response.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : response.substr(p + 4);
}

std::filesystem::path upload_dir() {
    return std::filesystem::temp_directory_path() / "orbit-request-bodies-test";
}

size_t files_in_upload_dir() {
    size_t n = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(upload_dir(), ec); !ec && it != std::filesystem::directory_iterator(); ++it) ++n;
    return n;
}

std::string multipart_body(const std::string& boundary, bool close) {
    std::string b;
    b += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nHoliday\r\n";
    b += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"a.jpg\"\r\n"
         "Content-Type: image/jpeg\r\n\r\n" + std::string(5000, 'J') + "\r\n";
    b += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"notes\"; filename=\"n.txt\"\r\n"
         "Content-Type: text/plain\r\n\r\nline one\nline two\r\n";
    if (close) b += "--" + boundary + "--\r\n";
    return b;
}

std::string upload_request(const std::string& content_type, const std::string& body) {
    return "POST /files/upload HTTP/1.1\r\nHost: x\r\nContent-Type: " + content_type +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

} // namespace

class RequestBodiesTest : public ::testing::Test {
protected:
    static server::App* app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        std::filesystem::remove_all(upload_dir());
        std::filesystem::create_directories(upload_dir());

        config::ServerConfig cfg;
        cfg.port = kPort;
        cfg.max_body_size = kMaxBody;
        app = new server::App(cfg);

        app->post("/echo", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body(std::string(req.body));
            w->send(std::move(res));
        });
        app->post("/form", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            auto fields = req.form_fields();
            http::HttpResponse res;
            res.set_body("name=" + fields["name"] + " lang=" + fields["lang"]);
            w->send(std::move(res));
        });
        app->group("/files", [](routing::Router& r) {
            r.add_stream_route(http::HttpMethod::POST, "/upload",
                [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
                    http::MultipartLimits limits;
                    limits.upload_dir = upload_dir().string();
                    limits.max_file_size = 8000;
                    http::receive_multipart(req, w, [w](http::MultipartUpload& upload) {
                        http::HttpResponse res;
                        if (!upload.ok()) {
                            res.status(http::HttpStatus::BadRequest);
                            res.set_body("error: " + upload.error);
                        } else {
                            std::string summary = "title=" + upload.fields["title"];
                            for (const auto& f : upload.files) {
                                std::ifstream in(f.path, std::ios::binary);
                                std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                                summary += " " + f.field + ":" + f.filename + ":" + f.content_type + ":" +
                                           std::to_string(content.size());
                            }
                            upload.discard();
                            res.set_body(summary);
                        }
                        w->send(std::move(res));
                    }, limits);
                });
        });

        server_thread = std::thread([] { app->listen(); });
        for (int i = 0; i < 200; ++i) {
            network::socket_t fd = connect_to();
            if (fd != network::INVALID_SOCKET_FD) {
                network::close_socket(fd);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    static void TearDownTestSuite() {
        app->stop();
        network::socket_t fd = connect_to(); // wake the loop
        if (fd != network::INVALID_SOCKET_FD) network::close_socket(fd);
        if (server_thread.joinable()) server_thread.join();
        delete app;
        std::filesystem::remove_all(upload_dir());
    }
};

server::App* RequestBodiesTest::app = nullptr;
std::thread RequestBodiesTest::server_thread;

TEST_F(RequestBodiesTest, ExpectContinueGets100BeforeTheBody) {
    network::socket_t fd = connect_to();
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    send_str(fd, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n");
    std::string interim = read_some(fd);
    EXPECT_EQ(interim, "HTTP/1.1 100 Continue\r\n\r\n");
    send_str(fd, "hello");
    std::string res = read_response(fd);
    network::close_socket(fd);
    EXPECT_EQ(res.rfind("HTTP/1.1 200", 0), 0u) << res;
    EXPECT_EQ(body_of(res), "hello");
}

TEST_F(RequestBodiesTest, ExpectContinueOncePerRequestOnKeepAlive) {
    network::socket_t fd = connect_to();
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    for (const char* word : {"one", "two"}) {
        send_str(fd, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n");
        EXPECT_EQ(read_some(fd), "HTTP/1.1 100 Continue\r\n\r\n") << word;
        send_str(fd, word);
        std::string res = read_response(fd);
        EXPECT_EQ(body_of(res), word) << res;
        EXPECT_EQ(res.find("100 Continue"), std::string::npos) << res;
    }
    network::close_socket(fd);
}

TEST_F(RequestBodiesTest, BodySentWithTheHeadersGetsNo100) {
    network::socket_t fd = connect_to();
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    send_str(fd, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\nExpect: 100-continue\r\n\r\nhi");
    std::string res = read_response(fd);
    network::close_socket(fd);
    EXPECT_EQ(res.rfind("HTTP/1.1 200", 0), 0u) << res;
    EXPECT_EQ(body_of(res), "hi");
}

TEST_F(RequestBodiesTest, Http10ClientGetsNo100) {
    network::socket_t fd = connect_to(400);
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    send_str(fd, "POST /echo HTTP/1.0\r\nHost: x\r\nContent-Length: 2\r\nExpect: 100-continue\r\n\r\n");
    EXPECT_EQ(read_some(fd), ""); // nothing until the body arrives
    send_str(fd, "ok");
    std::string res = read_response(fd);
    network::close_socket(fd);
    EXPECT_EQ(body_of(res), "ok") << res;
}

TEST_F(RequestBodiesTest, OversizedBodyGets413Instead) {
    network::socket_t fd = connect_to();
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    send_str(fd, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(kMaxBody + 1) +
                 "\r\nExpect: 100-continue\r\n\r\n");
    std::string res = read_response(fd);
    network::close_socket(fd);
    EXPECT_EQ(res.rfind("HTTP/1.1 413", 0), 0u) << res;
}

TEST_F(RequestBodiesTest, FormFieldsOverTheWire) {
    network::socket_t fd = connect_to();
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    std::string body = "name=Grace+Hopper&lang=COBOL%21";
    send_str(fd, "POST /form HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                 "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    std::string res = read_response(fd);
    network::close_socket(fd);
    EXPECT_EQ(body_of(res), "name=Grace Hopper lang=COBOL!") << res;
}

TEST_F(RequestBodiesTest, MultipartUploadIsSavedToDisk) {
    network::socket_t fd = connect_to();
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    send_str(fd, upload_request("multipart/form-data; boundary=\"XyZ\"", multipart_body("XyZ", true)));
    std::string res = read_response(fd);
    network::close_socket(fd);
    EXPECT_EQ(res.rfind("HTTP/1.1 200", 0), 0u) << res;
    EXPECT_EQ(body_of(res), "title=Holiday photo:a.jpg:image/jpeg:5000 notes:n.txt:text/plain:17");
    EXPECT_EQ(files_in_upload_dir(), 0u); // the handler discarded them
}

TEST_F(RequestBodiesTest, StreamRouteSends100Continue) {
    network::socket_t fd = connect_to();
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    std::string body = multipart_body("b", true);
    send_str(fd, "POST /files/upload HTTP/1.1\r\nHost: x\r\nContent-Type: multipart/form-data; boundary=b\r\n"
                 "Content-Length: " + std::to_string(body.size()) + "\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n");
    EXPECT_EQ(read_some(fd), "HTTP/1.1 100 Continue\r\n\r\n");
    send_str(fd, body);
    std::string res = read_response(fd);
    network::close_socket(fd);
    EXPECT_EQ(res.rfind("HTTP/1.1 200", 0), 0u) << res;
}

TEST_F(RequestBodiesTest, TruncatedUploadFailsAndLeavesNoFiles) {
    network::socket_t fd = connect_to();
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    send_str(fd, upload_request("multipart/form-data; boundary=XyZ", multipart_body("XyZ", false)));
    std::string res = read_response(fd);
    network::close_socket(fd);
    EXPECT_EQ(res.rfind("HTTP/1.1 400", 0), 0u) << res;
    EXPECT_EQ(body_of(res), "error: multipart body ended before its closing boundary");
    EXPECT_EQ(files_in_upload_dir(), 0u);
}

TEST_F(RequestBodiesTest, FileLimitFailsAndLeavesNoFiles) {
    std::string body = "--L\r\nContent-Disposition: form-data; name=\"ok\"; filename=\"ok.bin\"\r\n\r\n" +
                       std::string(100, 'o') + "\r\n--L\r\nContent-Disposition: form-data; name=\"big\"; filename=\"big.bin\"\r\n\r\n" +
                       std::string(9000, 'x') + "\r\n--L--\r\n";
    network::socket_t fd = connect_to();
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    send_str(fd, upload_request("multipart/form-data; boundary=L", body));
    std::string res = read_response(fd);
    network::close_socket(fd);
    EXPECT_EQ(res.rfind("HTTP/1.1 400", 0), 0u) << res;
    EXPECT_NE(body_of(res).find("error: "), std::string::npos) << res;
    EXPECT_EQ(files_in_upload_dir(), 0u) << "the completed first file must be deleted too";
}

TEST_F(RequestBodiesTest, NonMultipartUploadIsRejected) {
    network::socket_t fd = connect_to();
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    send_str(fd, upload_request("application/json", "{}"));
    std::string res = read_response(fd);
    network::close_socket(fd);
    EXPECT_EQ(res.rfind("HTTP/1.1 400", 0), 0u) << res;
    EXPECT_EQ(body_of(res), "error: expected multipart/form-data with a boundary");
}
