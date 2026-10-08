#include <orbit/http/ResponseWriter.hpp>
#include <orbit/utils/Logger.hpp>

#include <string>

namespace orbit::http {

namespace {

std::string describe(std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown exception";
    }
}

} // namespace

void ResponseWriter::report_async_exception(const std::shared_ptr<ResponseWriter>& writer, uint64_t generation,
                                            std::exception_ptr error) noexcept {
    try {
        if (!writer) {
            LOG_ERROR("Unhandled exception in a coroutine: " << describe(error));
            return;
        }
        if (generation != writer->request_generation()) {
            // The request finished and the connection is serving another
            // one; answering now would answer the wrong request.
            LOG_ERROR("Unhandled exception in a coroutine after its request finished: " << describe(error));
            return;
        }
        if (writer->has_responded()) {
            // The response (or part of it) is out. HTTP/2 and HTTP/3 free a
            // stream's request once it completes, so the sink, which reads
            // the request, must not run; a second response is impossible anyway.
            LOG_ERROR("Unhandled exception in a coroutine after it responded: " << describe(error));
            return;
        }
        ErrorSink sink;
        {
            std::lock_guard<std::mutex> lock(writer->error_sink_mutex_);
            sink = writer->error_sink_;
        }
        if (sink) {
            sink(error, writer);
            return;
        }
        // A writer used outside the router (no sink): still answer.
        LOG_ERROR("Unhandled exception in an asynchronous handler: " << describe(error));
        if (!writer->has_responded()) {
            HttpResponse res;
            res.status(HttpStatus::InternalServerError).send("500 Internal Server Error");
            writer->send(std::move(res));
        }
    } catch (...) {
        LOG_ERROR("Failed to report an exception from an asynchronous handler");
    }
}

} // namespace http
