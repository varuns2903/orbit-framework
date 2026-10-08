#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <string_view>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <orbit/http/json.hpp>

namespace orbit::network { class Proactor; }
namespace orbit::concurrency { class ThreadPool; }

namespace orbit::http {

/**
 * @brief Abstract interface for writing HTTP responses.
 * @details This is used by middlewares and route handlers to write data back to the client.
 */
class ResponseWriter {
public:
    virtual ~ResponseWriter() = default;

    using Interceptor = std::function<void(HttpResponse&)>;
    /**
     * @brief Adds an interceptor to modify the response before it is sent.
     * @param interceptor The interceptor callback.
     */
    virtual void add_interceptor(Interceptor interceptor) = 0;

    /**
     * @brief Adds a default header that will be included in the final response.
     * @param key The header name.
     * @param value The header value.
     */
    virtual void set_header(const std::string& key, const std::string& value) = 0;

    /**
     * @brief Gets the underlying Proactor to dispatch async operations.
     * @return A reference to the network::Proactor.
     */
    virtual network::Proactor& proactor() = 0;
    
    /**
     * @brief Gets the thread pool to offload blocking tasks.
     * @return A reference to the concurrency::ThreadPool.
     */
    virtual concurrency::ThreadPool& thread_pool() = 0;

    /**
     * @brief Sends a complete HTTP response.
     * @details Takes ownership of any file descriptors managed by the response.
     * @param response The HttpResponse to send.
     */
    virtual void send(HttpResponse&& response) = 0;

    /**
     * @brief Sends only the HTTP headers.
     * @details Useful for streaming bodies or Server-Sent Events.
     * @param response The HttpResponse containing the headers to send.
     */
    virtual void send_headers(HttpResponse& response) = 0;

    /**
     * @brief Streams a chunk of data (for Chunked Transfer Encoding).
     * @param chunk The data chunk to write.
     */
    virtual void write_chunk(std::string_view chunk) = 0;

    /**
     * @brief Ends a chunked response stream.
     */
    virtual void end() = 0;

    /**
     * @brief Sends a Server-Sent Events (SSE) message.
     * @param data The event data payload.
     * @param event The event type/name (optional).
     * @param id The event ID (optional).
     */
    virtual void send_sse_event(std::string_view data, std::string_view event = "", std::string_view id = "") = 0;

    /**
     * @brief Upgrades the connection to a raw bi-directional byte stream.
     * @param on_data Callback invoked when data is received.
     * @param on_close Callback invoked when the stream is closed.
     */
    virtual void upgrade_to_raw_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_close) = 0;
    
    /**
     * @brief Asynchronously streams the incoming HTTP request body.
     * @param on_data Callback invoked when a body chunk is received.
     * @param on_end Callback invoked when the entire body has been read.
     */
    virtual void read_body_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_end) = 0;

    /**
     * @brief True once anything of a response to the current request has
     *        been sent: a full response, its headers, a chunk or an event.
     *
     * The router reads it after an error handler fails, to answer the
     * request with a 500 only if nothing has been sent yet.
     */
    bool has_responded() const { return responded_.load(); }

    /**
     * @brief Receives an exception that escaped asynchronous handler code,
     *        such as a coroutine handler that threw after a co_await.
     *
     * The router installs one for each request; it runs the same error
     * handling as for a handler that throws synchronously.
     */
    using ErrorSink = std::function<void(std::exception_ptr, std::shared_ptr<ResponseWriter>)>;
    void set_error_sink(ErrorSink sink) {
        std::lock_guard<std::mutex> lock(error_sink_mutex_);
        error_sink_ = std::move(sink);
    }

    /// Identifies the request this writer is answering. A writer reused
    /// across requests (an HTTP/1.1 keep-alive connection) advances it with
    /// each request, so late failures of an earlier one can be told apart.
    uint64_t request_generation() const { return generation_.load(); }

    /**
     * @brief Reports an exception from asynchronous handler code.
     *
     * Goes through the request's error sink (the router's on_error handler,
     * else a 500). The exception is only logged if part of the response has
     * already been sent, or if @p generation is no longer the writer's
     * current request (the connection moved on to another). Never throws.
     */
    static void report_async_exception(const std::shared_ptr<ResponseWriter>& writer, uint64_t generation,
                                       std::exception_ptr error) noexcept;

protected:
    /// Implementations call this whenever they write part of a response.
    void mark_responded() { responded_ = true; }
    /// For writers reused across requests (an HTTP/1.1 keep-alive
    /// connection), at the start of each request.
    void reset_responded() {
        responded_ = false;
        ++generation_;
        std::lock_guard<std::mutex> lock(error_sink_mutex_);
        error_sink_ = nullptr;
    }

private:
    std::atomic<bool> responded_{false};
    std::atomic<uint64_t> generation_{0};
    std::mutex error_sink_mutex_;
    ErrorSink error_sink_;
};

} // namespace http
