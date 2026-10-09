#include <gtest/gtest.h>
#include <orbit/http/ResponseWriter.hpp>

#include <stdexcept>
#include <string>
#include <vector>

// ResponseWriter::on_close() / is_open() semantics (#197).

using namespace orbit::http;

namespace {

class ClosableWriter : public ResponseWriter {
public:
    using ResponseWriter::mark_closed;
    void send(HttpResponse&&) override {}
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(Interceptor) override {}
    void set_header(const std::string&, const std::string&) override {}
    orbit::network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    orbit::concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

} // namespace

TEST(WriterCloseUnitTest, CallbacksRunOnceInOrder) {
    ClosableWriter w;
    std::vector<int> order;
    w.on_close([&] { order.push_back(1); });
    w.on_close([&] { order.push_back(2); });
    EXPECT_TRUE(w.is_open());
    EXPECT_TRUE(order.empty());

    w.mark_closed();
    EXPECT_FALSE(w.is_open());
    EXPECT_EQ(order, (std::vector<int>{1, 2}));

    w.mark_closed(); // again: nothing more
    EXPECT_EQ(order.size(), 2u);
}

TEST(WriterCloseUnitTest, LateRegistrationRunsAtOnce) {
    ClosableWriter w;
    w.mark_closed();
    bool ran = false;
    w.on_close([&] { ran = true; });
    EXPECT_TRUE(ran);
}

TEST(WriterCloseUnitTest, AThrowingCallbackDoesNotStopTheOthers) {
    ClosableWriter w;
    bool second = false;
    w.on_close([] { throw std::runtime_error("callback failed"); });
    w.on_close([&] { second = true; });
    EXPECT_NO_THROW(w.mark_closed());
    EXPECT_TRUE(second);
    EXPECT_NO_THROW(w.on_close([] { throw 1; }));
}

// A callback may register another; it runs at once, since the writer is
// already closed by then.
TEST(WriterCloseUnitTest, ACallbackCanRegisterAnother) {
    ClosableWriter w;
    bool inner = false;
    w.on_close([&] { w.on_close([&] { inner = true; }); });
    w.mark_closed();
    EXPECT_TRUE(inner);
}
