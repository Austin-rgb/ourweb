#include "ourweb/middleware.hpp"

#include "chain.hpp"
#include "ourweb/http.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace ourweb {

namespace detail {

Response ChainState::advance(std::size_t index) {
    if (index != cursor_) {
        throw std::logic_error(
            "ourweb: next() may be called at most once, from within Middleware::invoke");
    }
    cursor_ = index + 1;

    try {
        if (index < middlewares_.size()) {
            return middlewares_[index]->invoke(request_, MiddlewareNext(this, index + 1));
        }
        return terminal();
    } catch (const std::exception& e) {
        std::cerr << "[ourweb] unhandled exception in "
                  << (index < middlewares_.size() ? "middleware" : "handler") << ": " << e.what()
                  << '\n';
    } catch (...) {
        std::cerr << "[ourweb] unhandled non-standard exception in "
                  << (index < middlewares_.size() ? "middleware" : "handler") << '\n';
    }
    return internal_error();
}

Response ChainState::terminal() const {
    if (handler_) {
        return handler_(request_);
    }
    if (path_matched_) {
        Response resp = method_not_allowed();
        std::string allow;
        auto append = [&allow](HttpMethod m) {
            if (!allow.empty()) allow += ", ";
            allow += std::string(to_string(m));
        };
        const bool has_head = std::find(allowed_methods_.begin(), allowed_methods_.end(),
                                        HttpMethod::Head) != allowed_methods_.end();
        for (const HttpMethod m : allowed_methods_) {
            append(m);
            if (m == HttpMethod::Get && !has_head) append(HttpMethod::Head);
        }
        if (!allow.empty()) resp.header("Allow", std::move(allow));
        return resp;
    }
    return not_found();
}

} // namespace detail

Response MiddlewareNext::operator()() const {
    if (fn_) return (*fn_)();
    if (!chain_) {
        throw std::logic_error("ourweb: empty MiddlewareNext invoked");
    }
    return chain_->advance(index_);
}

LoggerMiddleware::LoggerMiddleware(std::string prefix) : prefix_(std::move(prefix)) {}

Response LoggerMiddleware::invoke(const Request& req, MiddlewareNext next) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();

    Response resp = next();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start);

    std::ostringstream line;
    line << prefix_ << ' ' << to_string(req.method()) << ' ' << req.path() << " -> "
         << static_cast<int>(resp.status_code()) << " (" << elapsed.count() << " ms)\n";

    // One locked write per line so concurrent requests cannot interleave output.
    static std::mutex mu;
    const std::lock_guard<std::mutex> lock(mu);
    std::cout << line.str();
    return resp;
}

} // namespace ourweb
