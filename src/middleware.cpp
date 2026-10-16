#include "ourweb/middleware.hpp"
#include "ourweb/http.hpp"

#include <chrono>
#include <iostream>

namespace ourweb {

LoggerMiddleware::LoggerMiddleware(std::string prefix) : prefix_(std::move(prefix)) {}

Response LoggerMiddleware::invoke(const Request& req, MiddlewareNext next) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();

    Response resp = next();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start);

    std::cout << prefix_ << ' '
              << to_string(req.method()) << ' '
              << req.path() << " -> "
              << static_cast<int>(resp.status_code())
              << " (" << elapsed.count() << " ms)\n";

    return resp;
}

} // namespace ourweb
