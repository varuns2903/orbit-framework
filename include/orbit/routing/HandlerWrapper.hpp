#pragma once
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <nlohmann/json.hpp>
#include <type_traits>
#include <functional>
#include <memory>
#include <string>
#include <tuple>

namespace routing {

// Function traits to inspect lambda signatures
template <typename T>
struct function_traits : public function_traits<decltype(&std::decay_t<T>::operator())> {};

// Specialization for const lambdas
template <typename ClassType, typename ReturnType, typename... Args>
struct function_traits<ReturnType(ClassType::*)(Args...) const> {
    using result_type = ReturnType;
    using args_tuple = std::tuple<Args...>;
    static constexpr size_t arg_count = sizeof...(Args);
};

// Specialization for mutable lambdas
template <typename ClassType, typename ReturnType, typename... Args>
struct function_traits<ReturnType(ClassType::*)(Args...)> {
    using result_type = ReturnType;
    using args_tuple = std::tuple<Args...>;
    static constexpr size_t arg_count = sizeof...(Args);
};

// Specialization for free functions
template <typename ReturnType, typename... Args>
struct function_traits<ReturnType(*)(Args...)> {
    using result_type = ReturnType;
    using args_tuple = std::tuple<Args...>;
    static constexpr size_t arg_count = sizeof...(Args);
};

// The core wrapper
template <typename Handler>
auto wrap_handler(Handler&& h) -> std::function<void(http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)> {
    using traits = function_traits<Handler>;
    using Ret = typename traits::result_type;

    if constexpr (std::is_same_v<Ret, void>) {
        // It's a standard handler, we just pass it through.
        // Assuming it takes (HttpRequest&, shared_ptr<ResponseWriter>)
        return [h = std::forward<Handler>(h)](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) mutable {
            h(req, writer);
        };
    } else {
        // It returns a value! Let's auto-serialize it.
        return [h = std::forward<Handler>(h)](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) mutable {
            try {
                Ret result;
                if constexpr (traits::arg_count == 0) {
                    result = h();
                } else if constexpr (traits::arg_count == 1) {
                    result = h(req);
                } else {
                    static_assert(traits::arg_count <= 1, "Magic handlers can only take 0 or 1 arguments (HttpRequest&)");
                }

                http::HttpResponse res;
                if constexpr (std::is_same_v<Ret, std::string>) {
                    res.status(http::HttpStatus::OK).send(result);
                    res.headers["Content-Type"] = "text/plain";
                } else if constexpr (std::is_same_v<Ret, nlohmann::json>) {
                    res.status(http::HttpStatus::OK).send(result.dump());
                    res.headers["Content-Type"] = "application/json";
                } else {
                    // It must be a C++ struct. nlohmann::json will serialize it!
                    nlohmann::json j = result;
                    res.status(http::HttpStatus::OK).send(j.dump());
                    res.headers["Content-Type"] = "application/json";
                }
                writer->send(std::move(res));
            } catch (const std::exception& e) {
                http::HttpResponse res;
                res.status(http::HttpStatus::InternalServerError).send(e.what());
                res.headers["Content-Type"] = "text/plain";
                writer->send(std::move(res));
            }
        };
    }
}

} // namespace routing
