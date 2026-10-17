#include "ourweb/router.hpp"

#include "url.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace ourweb {

namespace {

// Splits a path into segments. Exactly one trailing '/' is ignored; empty
// interior segments are preserved (they never match anything).
std::vector<std::string_view> split_segments(std::string_view path) {
    std::vector<std::string_view> segments;
    if (path.empty() || path == "/") return segments;
    if (path.front() == '/') path.remove_prefix(1);
    if (!path.empty() && path.back() == '/') path.remove_suffix(1);
    std::size_t start = 0;
    for (;;) {
        const auto pos = path.find('/', start);
        if (pos == std::string_view::npos) {
            segments.push_back(path.substr(start));
            break;
        }
        segments.push_back(path.substr(start, pos - start));
        start = pos + 1;
    }
    return segments;
}

bool is_identifier(std::string_view s) {
    if (s.empty()) return false;
    const auto first = static_cast<unsigned char>(s.front());
    if (!(std::isalpha(first) || first == '_')) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        const auto uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) || c == '_';
    });
}

[[noreturn]] void bad_pattern(std::string_view pattern, const std::string& why) {
    throw RouteError("Invalid route pattern \"" + std::string(pattern) + "\": " + why);
}

constexpr std::array<HttpMethod, 9> kMethodOrder = {
    HttpMethod::Get,     HttpMethod::Head,   HttpMethod::Post,
    HttpMethod::Put,     HttpMethod::Patch,  HttpMethod::Delete,
    HttpMethod::Options, HttpMethod::Trace,  HttpMethod::Connect};

} // namespace

std::vector<std::string> Router::validate_pattern(std::string_view pattern) {
    if (pattern.empty() || pattern.front() != '/') {
        bad_pattern(pattern, "must start with '/'");
    }
    for (char c : pattern) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || uc == 0x7F) bad_pattern(pattern, "whitespace/control characters are not allowed");
        if (c == '?' || c == '#') bad_pattern(pattern, "query strings and fragments are not part of a route");
        if (c == '%') bad_pattern(pattern, "percent-encoding is not allowed; write the decoded text");
        if (c == '*') bad_pattern(pattern, "wildcard routes are not supported");
    }
    if (pattern.find("//") != std::string_view::npos) {
        bad_pattern(pattern, "empty path segment");
    }

    std::vector<std::string> names;
    std::string_view rest = pattern;
    rest.remove_prefix(1);
    if (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);
    if (rest.empty()) return names;  // "/" (or "//" which was rejected above)

    std::size_t start = 0;
    for (;;) {
        const auto pos = rest.find('/', start);
        const auto seg = (pos == std::string_view::npos) ? rest.substr(start)
                                                          : rest.substr(start, pos - start);
        const bool has_open = seg.find('{') != std::string_view::npos;
        const bool has_close = seg.find('}') != std::string_view::npos;
        if (has_open || has_close) {
            if (seg.size() < 3 || seg.front() != '{' || seg.back() != '}' ||
                seg.find('{', 1) != std::string_view::npos ||
                seg.find('}') != seg.size() - 1) {
                bad_pattern(pattern, "malformed parameter segment \"" + std::string(seg) +
                                         "\"; a parameter must be exactly \"{name}\"");
            }
            const auto name = seg.substr(1, seg.size() - 2);
            if (!is_identifier(name)) {
                bad_pattern(pattern, "parameter name \"" + std::string(name) +
                                         "\" must match [A-Za-z_][A-Za-z0-9_]*");
            }
            if (std::find(names.begin(), names.end(), name) != names.end()) {
                bad_pattern(pattern, "duplicate parameter name \"" + std::string(name) + "\"");
            }
            names.emplace_back(name);
        }
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return names;
}

void Router::add(HttpMethod method, std::string pattern, HandlerFn handler,
                 std::size_t required_path_params) {
    if (method == HttpMethod::Unknown) {
        throw RouteError("Cannot register a route for an unknown HTTP method");
    }
    if (!handler) {
        throw RouteError("Cannot register an empty handler for " + pattern);
    }

    CompiledRoute route;
    route.param_names = validate_pattern(pattern);
    if (required_path_params > route.param_names.size()) {
        throw RouteError("Handler for " + std::string(to_string(method)) + " " + pattern +
                         " extracts " + std::to_string(required_path_params) +
                         " path parameter(s) but the pattern declares " +
                         std::to_string(route.param_names.size()));
    }

    route.method = method;
    route.original_pattern = pattern;
    route.shape = std::string(to_string(method)) + ' ';
    for (const auto seg : split_segments(pattern)) {
        Segment s;
        if (!seg.empty() && seg.front() == '{') {
            s.is_param = true;
            s.text = std::string(seg.substr(1, seg.size() - 2));
            route.shape += "/{}";
        } else {
            s.text = std::string(seg);
            route.shape += '/';
            route.shape += seg;
        }
        route.segments.push_back(std::move(s));
    }
    if (route.segments.empty()) route.shape += '/';

    for (const auto& existing : routes_) {
        if (existing.shape == route.shape) {
            throw RouteError("Duplicate route: " + std::string(to_string(method)) + ' ' + pattern +
                             " conflicts with " + std::string(to_string(existing.method)) + ' ' +
                             existing.original_pattern);
        }
    }

    route.handler = std::move(handler);
    routes_.push_back(std::move(route));

    // Specificity order: fewer segments first (irrelevant to matching, since
    // only equal lengths can match), then literal-before-parameter at the first
    // differing position. stable_sort keeps registration order for ties.
    std::stable_sort(routes_.begin(), routes_.end(), [](const CompiledRoute& a, const CompiledRoute& b) {
        if (a.segments.size() != b.segments.size()) return a.segments.size() < b.segments.size();
        for (std::size_t i = 0; i < a.segments.size(); ++i) {
            if (a.segments[i].is_param != b.segments[i].is_param) return !a.segments[i].is_param;
        }
        return false;
    });
}

Router::MatchResult Router::match(HttpMethod method, std::string_view path, Request& req) const {
    MatchResult result;

    // Decode each segment individually; a malformed escape cannot match anything.
    const auto raw_segments = split_segments(path);
    std::vector<std::string> segments;
    segments.reserve(raw_segments.size());
    for (const auto raw : raw_segments) {
        auto decoded = percent_decode_path(raw);
        if (!decoded) return result;
        segments.push_back(std::move(*decoded));
    }

    std::array<bool, kMethodOrder.size()> seen{};
    auto note_method = [&](HttpMethod m) {
        for (std::size_t i = 0; i < kMethodOrder.size(); ++i) {
            if (kMethodOrder[i] == m) seen[i] = true;
        }
    };

    const CompiledRoute* exact = nullptr;
    const CompiledRoute* head_fallback = nullptr;

    for (const auto& route : routes_) {
        if (route.segments.size() != segments.size()) continue;
        bool ok = true;
        for (std::size_t i = 0; i < segments.size() && ok; ++i) {
            const auto& rs = route.segments[i];
            ok = rs.is_param ? !segments[i].empty() : rs.text == segments[i];
        }
        if (!ok) continue;

        result.path_matched = true;
        note_method(route.method);

        if (route.method == method) {
            exact = &route;
            break;
        }
        if (method == HttpMethod::Head && route.method == HttpMethod::Get && !head_fallback) {
            head_fallback = &route;
        }
    }

    const CompiledRoute* chosen = exact ? exact : head_fallback;
    if (chosen) {
        PathParams params;
        params.reserve(chosen->param_names.size());
        std::size_t next_name = 0;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            if (chosen->segments[i].is_param) {
                params.emplace_back(chosen->param_names[next_name++], std::move(segments[i]));
            }
        }
        req.set_path_params(std::move(params));
        result.handler = HandlerRef(&chosen->handler);
    }

    for (std::size_t i = 0; i < kMethodOrder.size(); ++i) {
        if (seen[i]) result.allowed_methods.push_back(kMethodOrder[i]);
    }
    return result;
}

} // namespace ourweb
