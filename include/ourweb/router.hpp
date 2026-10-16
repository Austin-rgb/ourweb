#pragma once

#include "ourweb/http.hpp"
#include "ourweb/request.hpp"
#include "ourweb/response.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ourweb {

using HandlerFn = std::function<Response(Request)>;

struct Route {
    HttpMethod method;
    std::string pattern;           // e.g. "/users/{id}"
    std::vector<std::string> param_names;
    HandlerFn handler;
};

class Router {
public:
    void add(HttpMethod method, std::string pattern, HandlerFn handler);

    // Returns the matched handler and fills path params into the request,
    // or nullopt if no match.
    // Also returns allowed methods for 405 handling when path matches but method does not.
    struct MatchResult {
        HandlerFn handler;
        std::vector<HttpMethod> allowed_methods; // for 405
        bool path_matched = false;
    };

    [[nodiscard]] MatchResult match(HttpMethod method, std::string_view path, Request& req) const;

    [[nodiscard]] bool empty() const noexcept { return routes_.empty(); }

private:
    struct CompiledRoute {
        HttpMethod method;
        std::vector<std::string> segments; // literal or "{name}"
        std::vector<std::string> param_names;
        HandlerFn handler;
        std::string original_pattern;
    };

    static CompiledRoute compile(HttpMethod method, std::string pattern, HandlerFn handler);
    static bool match_segments(const CompiledRoute& route,
                               const std::vector<std::string_view>& path_segments,
                               std::unordered_map<std::string, std::string>& out_params);

    std::vector<CompiledRoute> routes_;
};

} // namespace ourweb
