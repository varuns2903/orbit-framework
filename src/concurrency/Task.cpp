#include <orbit/concurrency/Task.hpp>
#include <orbit/http/ResponseWriter.hpp>

namespace orbit::concurrency::detail {

uint64_t writer_generation(const std::shared_ptr<http::ResponseWriter>& writer) noexcept {
    return writer ? writer->request_generation() : 0;
}

void report_task_exception(const std::shared_ptr<http::ResponseWriter>& writer, uint64_t generation,
                           std::exception_ptr error) noexcept {
    // Without a writer this only logs; with one it reaches the router's
    // error handling for the request the coroutine was started for.
    http::ResponseWriter::report_async_exception(writer, generation, std::move(error));
}

} // namespace orbit::concurrency::detail
