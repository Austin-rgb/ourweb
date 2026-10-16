#pragma once

#include "ourweb/request.hpp"
#include "ourweb/response.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ourweb {

// Middleware can short-circuit by returning a Response, or continue by returning nullopt
using MiddlewareNext = std::function<Response()>;
using MiddlewareFn = std::function<Response(const Request&, MiddlewareNext)>;

class Middleware {
public:
    virtual ~Middleware() = default;

    // Called before the handler. Return a Response to short-circuit, or nullopt to continue.
    // After the next() call, the middleware can inspect/modify the response.
    virtual Response invoke(const Request& req, MiddlewareNext next) = 0;
};

// Simple logging middleware
class LoggerMiddleware : public Middleware {
public:
    explicit LoggerMiddleware(std::string prefix = "[ourweb]");

    Response invoke(const Request& req, MiddlewareNext next) override;

private:
    std::string prefix_;
};

// Function-based middleware adapter
class FunctionMiddleware : public Middleware {
public:
    explicit FunctionMiddleware(MiddlewareFn fn) : fn_(std::move(fn)) {}

    Response invoke(const Request& req, MiddlewareNext next) override {
        return fn_(req, std::move(next));
    }

private:
    MiddlewareFn fn_;
};

} // namespace ourweb
