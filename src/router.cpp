#include "ourweb/router.hpp"

#include <sstream>
#include <stdexcept>

namespace ourweb {

namespace {

std::vector<std::string_view> split_path(std::string_view path) {
    std::vector<std::string_view> segments;
    if (path.empty() || path == "/") {
        return segments; // empty for root
    }
    // Skip leading '/'
    std::size_t start = (path.front() == '/') ? 1 : 0;
    while (start < path.size()) {
        auto pos = path.find('/', start);
        if (pos == std::string_view::npos) {
            segments.push_back(path.substr(start));
            break;
        }
        if (pos > start) {
            segments.push_back(path.substr(start, pos - start));
        }
        start = pos + 1;
    }
    return segments;
}

bool is_param(std::string_view seg) {
    return seg.size() >= 3 && seg.front() == '{' && seg.back() == '}';
}

std::string param_name(std::string_view seg) {
    return std::string(seg.substr(1, seg.size() - 2));
}

} // namespace

Router::CompiledRoute Router::compile(HttpMethod method, std::string pattern, HandlerFn handler) {
    CompiledRoute cr;
    cr.method = method;
    cr.original_pattern = pattern;
    cr.handler = std::move(handler);

    auto segs = split_path(pattern);
    for (auto s : segs) {
        cr.segments.emplace_back(s);
        if (is_param(s)) {
            cr.param_names.push_back(param_name(s));
        }
    }
    return cr;
}

bool Router::match_segments(const CompiledRoute& route,
                            const std::vector<std::string_view>& path_segments,
                            std::unordered_map<std::string, std::string>& out_params) {
    if (route.segments.size() != path_segments.size()) {
        return false;
    }
    out_params.clear();
    for (std::size_t i = 0; i < route.segments.size(); ++i) {
        const auto& rs = route.segments[i];
        const auto& ps = path_segments[i];
        if (is_param(rs)) {
            out_params[param_name(rs)] = std::string(ps);
        } else if (rs != ps) {
            return false;
        }
    }
    return true;
}

void Router::add(HttpMethod method, std::string pattern, HandlerFn handler) {
    // Detect obvious duplicates (same method + exact pattern)
    for (const auto& r : routes_) {
        if (r.method == method && r.original_pattern == pattern) {
            throw std::runtime_error("Duplicate route registration: " +
                                     std::string(to_string(method)) + " " + pattern);
        }
    }
    routes_.push_back(compile(method, std::move(pattern), std::move(handler)));
}

Router::MatchResult Router::match(HttpMethod method, std::string_view path, Request& req) const {
    MatchResult result;
    auto path_segs = split_path(path);

    std::vector<HttpMethod> allowed;
    const CompiledRoute* head_fallback = nullptr; // GET route used for HEAD requests
    std::unordered_map<std::string, std::string> head_fallback_params;

    for (const auto& route : routes_) {
        std::unordered_map<std::string, std::string> params;
        if (match_segments(route, path_segs, params)) {
            result.path_matched = true;
            allowed.push_back(route.method);
            if (route.method == method) {
                req.set_path_params(std::move(params));
                result.handler = route.handler;
                result.allowed_methods = std::move(allowed);
                return result;
            }
            if (method == HttpMethod::Head && route.method == HttpMethod::Get && !head_fallback) {
                head_fallback = &route;
                head_fallback_params = std::move(params);
            }
        }
    }

    // HEAD falls back to the GET handler (the server strips the body).
    if (head_fallback) {
        req.set_path_params(std::move(head_fallback_params));
        result.handler = head_fallback->handler;
        result.allowed_methods = std::move(allowed);
        return result;
    }

    result.allowed_methods = std::move(allowed);
    return result;
}

} // namespace ourweb
