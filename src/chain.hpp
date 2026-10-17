#pragma once

// Internal: per-request middleware chain. Lives on the stack of
// App::handle_request; MiddlewareNext objects hold a pointer into it.

#include "ourweb/http.hpp"
#include "ourweb/middleware.hpp"
#include "ourweb/request.hpp"
#include "ourweb/response.hpp"
#include "ourweb/router.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace ourweb::detail {

class ChainState {
public:
    ChainState(const std::vector<std::shared_ptr<Middleware>>& middlewares,
               const Request& request,
               Router::HandlerRef handler,
               bool path_matched,
               const std::vector<HttpMethod>& allowed_methods) noexcept
        : middlewares_(middlewares),
          request_(request),
          handler_(handler),
          path_matched_(path_matched),
          allowed_methods_(allowed_methods) {}

    ChainState(const ChainState&) = delete;
    ChainState& operator=(const ChainState&) = delete;

    // Run the whole chain (outermost middleware first). Never throws.
    Response run() { return advance(0); }

    // Run stage `index`: middleware[index], or the terminal stage when index
    // equals the middleware count. Throws std::logic_error on out-of-order or
    // repeated calls; otherwise never throws (exceptions become 500).
    Response advance(std::size_t index);

private:
    Response terminal() const;

    const std::vector<std::shared_ptr<Middleware>>& middlewares_;
    const Request& request_;
    Router::HandlerRef handler_;
    bool path_matched_;
    const std::vector<HttpMethod>& allowed_methods_;
    std::size_t cursor_ = 0;  // index of the next stage allowed to run
};

} // namespace ourweb::detail
