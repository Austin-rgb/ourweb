#pragma once

#include "ourweb/http.hpp"
#include "ourweb/request.hpp"
#include "ourweb/response.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ourweb {

// Handlers are invoked with a borrowed request; see Request for lifetime rules.
using HandlerFn = std::function<Response(const Request&)>;

// Thrown by Router::add (and therefore App::get/post/...) for an invalid route
// pattern, a duplicate route, or a handler/pattern mismatch. Route registration
// is a startup-time activity, so these are meant to fail loudly and early.
class RouteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Route {
    HttpMethod method;
    std::string pattern;           // e.g. "/users/{id}"
    std::vector<std::string> param_names;
    HandlerFn handler;
};

// Route patterns
// --------------
//   * must start with '/'; a single trailing '/' is ignored ("/a/" == "/a")
//   * segments are separated by '/'; empty segments ("//") are rejected
//   * a segment is either literal text or exactly "{name}" where name matches
//     [A-Za-z_][A-Za-z0-9_]*; partial parameters ("a{id}", "{id}.json") are
//     rejected, as are duplicate parameter names
//   * '?', '#', '%', '*', whitespace and control characters are rejected
//     (query strings are not part of the route; wildcards are unsupported;
//     write literal segments in decoded form)
//
// Matching
// --------
//   * request path segments are percent-decoded individually before matching,
//     so "%2F" never acts as a separator; a parameter value is never empty
//   * a literal segment beats a parameter at the first position they differ,
//     independent of registration order ("/users/me" before "/users/{id}")
//   * two routes with the same method and the same *shape* are duplicates:
//     "/users/{id}" and "/users/{uid}" conflict, as do "/a" and "/a/"
//   * HEAD falls back to the matching GET route if no HEAD route matches
class Router {
public:
    // Non-owning reference to a registered handler. Valid while the Router is
    // alive and unmodified. Cheap to copy; avoids copying the std::function
    // (and whatever it captured) on every request.
    class HandlerRef {
    public:
        HandlerRef() = default;
        explicit operator bool() const noexcept { return fn_ != nullptr; }
        Response operator()(const Request& req) const { return (*fn_)(req); }

    private:
        friend class Router;
        explicit HandlerRef(const HandlerFn* fn) : fn_(fn) {}
        const HandlerFn* fn_ = nullptr;
    };

    // `required_path_params`: how many Path<T> extractors the handler uses.
    // Registration fails if the pattern has fewer parameters than that.
    void add(HttpMethod method, std::string pattern, HandlerFn handler,
             std::size_t required_path_params = 0);

    struct MatchResult {
        HandlerRef handler;
        // Methods actually registered for the matched path (deduplicated,
        // canonical order; the implicit HEAD-for-GET is not listed). Complete
        // only when no handler matched; used to build 405 "Allow" headers.
        std::vector<HttpMethod> allowed_methods;
        bool path_matched = false;
    };

    // Matches and, on success, stores the decoded path parameters in `req`.
    [[nodiscard]] MatchResult match(HttpMethod method, std::string_view path, Request& req) const;

    [[nodiscard]] bool empty() const noexcept { return routes_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return routes_.size(); }

    // Validates a pattern and returns its parameter names (in order).
    // Throws RouteError with a descriptive message when invalid.
    [[nodiscard]] static std::vector<std::string> validate_pattern(std::string_view pattern);

private:
    struct Segment {
        bool is_param = false;
        std::string text;   // literal text, or the parameter name
    };

    struct CompiledRoute {
        HttpMethod method = HttpMethod::Get;
        std::vector<Segment> segments;
        std::vector<std::string> param_names;
        HandlerFn handler;
        std::string original_pattern;
        std::string shape;  // method + pattern with parameter names erased
    };

    std::vector<CompiledRoute> routes_;   // kept sorted by specificity
};

} // namespace ourweb
