#include "http_parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace ourweb {

namespace {

bool is_tchar(char c) {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc)) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool is_token(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return is_tchar(c); });
}

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string_view trim_ows(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool is_valid_field_value(std::string_view v) {
    return std::all_of(v.begin(), v.end(), [](char c) {
        const auto uc = static_cast<unsigned char>(c);
        return (uc >= 0x20 || c == '\t') && uc != 0x7F;
    });
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

enum class Version { Http10, Http11, Unsupported, Malformed };

Version parse_version(std::string_view v) {
    if (v == "HTTP/1.1") return Version::Http11;
    if (v == "HTTP/1.0") return Version::Http10;
    // HTTP/<digit>.<digit> is well-formed but not something we speak.
    if (v.size() == 8 && v.substr(0, 5) == "HTTP/" && std::isdigit(static_cast<unsigned char>(v[5])) &&
        v[6] == '.' && std::isdigit(static_cast<unsigned char>(v[7]))) {
        return Version::Unsupported;
    }
    return Version::Malformed;
}

} // namespace

RequestParser::RequestParser(ParserLimits limits) : limits_(limits) {}

void RequestParser::fail(HttpStatus status, std::string code, std::string message) {
    state_ = State::Failed;
    result_ = ParseResult{};
    result_.error = true;
    result_.error_status = status;
    result_.error_code = std::move(code);
    result_.error_message = std::move(message);
    head_.clear();
    body_.clear();
}

// Scan newly received head bytes for the CRLFCRLF terminator, rejecting
// framing violations as early as possible. Incremental: never rescans bytes.
bool RequestParser::scan_head() {
    for (; scan_ < head_.size(); ++scan_) {
        const char c = head_[scan_];
        if (c == '\0') {
            fail(HttpStatus::BadRequest, "bad_request", "NUL byte in request head");
            return false;
        }
        // After a CR the next byte must be LF; every LF must follow a CR.
        if (scan_ > 0 && head_[scan_ - 1] == '\r' && c != '\n') {
            fail(HttpStatus::BadRequest, "bad_request", "Bare CR in request head");
            return false;
        }
        if (c == '\n') {
            if (scan_ == 0 || head_[scan_ - 1] != '\r') {
                fail(HttpStatus::BadRequest, "bad_request", "Bare LF in request head");
                return false;
            }
            if (scan_ >= 3 && head_.compare(scan_ - 3, 4, "\r\n\r\n") == 0) {
                head_end_ = scan_ + 1;
                return true;
            }
        }
    }
    return false;
}

RequestParser::State RequestParser::feed(std::string_view chunk) {
    if (state_ == State::Complete || state_ == State::Failed) return state_;
    received_ += chunk.size();

    if (state_ == State::Head) {
        head_.append(chunk);
        const bool found = scan_head();
        if (state_ == State::Failed) return state_;
        if (!found) {
            if (head_.size() >= limits_.max_header_bytes) {
                fail(HttpStatus::RequestHeaderFieldsTooLarge, "header_too_large",
                     "Request headers too large");
            }
            return state_;
        }
        if (head_end_ > limits_.max_header_bytes) {
            fail(HttpStatus::RequestHeaderFieldsTooLarge, "header_too_large",
                 "Request headers too large");
            return state_;
        }
        parse_head();
        if (state_ == State::Failed) return state_;

        // Bytes already received past the head belong to the body.
        if (content_length_ == 0) {
            finish();
            return state_;
        }
        const std::size_t available = head_.size() - head_end_;
        body_.reserve(std::min<std::size_t>(content_length_, 64 * 1024));
        body_.append(head_, head_end_, std::min(available, content_length_));
        head_.clear();
        head_.shrink_to_fit();
        if (body_.size() >= content_length_) finish();
        return state_;
    }

    // State::Body
    const std::size_t need = content_length_ - body_.size();
    body_.append(chunk.substr(0, std::min(need, chunk.size())));
    if (body_.size() >= content_length_) finish();
    return state_;
}

void RequestParser::parse_head() {
    const std::string_view head(head_.data(), head_end_ - 4);
    std::size_t pos = 0;
    auto next_line = [&]() -> std::string_view {
        const auto end = head.find("\r\n", pos);
        std::string_view line = (end == std::string_view::npos) ? head.substr(pos)
                                                                 : head.substr(pos, end - pos);
        pos = (end == std::string_view::npos) ? head.size() + 1 : end + 2;
        return line;
    };

    // --- Request line: METHOD SP target SP HTTP-version -----------------------
    const std::string_view line = next_line();
    const auto sp1 = line.find(' ');
    const auto sp2 = (sp1 == std::string_view::npos) ? sp1 : line.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
        fail(HttpStatus::BadRequest, "bad_request", "Malformed request line");
        return;
    }
    const std::string_view method_sv = line.substr(0, sp1);
    const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version_sv = line.substr(sp2 + 1);
    if (method_sv.empty() || target.empty() || version_sv.empty() ||
        version_sv.find(' ') != std::string_view::npos || !is_token(method_sv)) {
        fail(HttpStatus::BadRequest, "bad_request", "Malformed request line");
        return;
    }
    if (target.size() > limits_.max_target_bytes) {
        fail(HttpStatus::UriTooLong, "uri_too_long", "Request target too long");
        return;
    }

    method_ = parse_method(method_sv);
    if (method_ == HttpMethod::Unknown) {
        fail(HttpStatus::NotImplemented, "not_implemented", "Unsupported HTTP method");
        return;
    }

    const Version version = parse_version(version_sv);
    if (version == Version::Malformed) {
        fail(HttpStatus::BadRequest, "bad_request", "Malformed HTTP version");
        return;
    }
    if (version == Version::Unsupported) {
        fail(HttpStatus::HttpVersionNotSupported, "version_not_supported",
             "HTTP version not supported");
        return;
    }

    // --- Request target ---------------------------------------------------------
    for (char c : target) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || uc == 0x7F || c == '#') {
            fail(HttpStatus::BadRequest, "bad_request", "Invalid character in request target");
            return;
        }
    }
    std::string_view path_and_query = target;
    if (target.front() != '/') {
        if (target == "*" && method_ == HttpMethod::Options) {
            path_and_query = target;
        } else if (starts_with_ci(target, "http://") || starts_with_ci(target, "https://")) {
            // absolute-form (RFC 7230 5.3.2): drop scheme and authority.
            const auto scheme_end = target.find("://") + 3;
            const auto slash = target.find_first_of("/?", scheme_end);
            // "http://host?x=1" yields "?x=1": empty path, normalised to "/" below.
            path_and_query = (slash == std::string_view::npos) ? std::string_view("/")
                                                                : target.substr(slash);
        } else {
            fail(HttpStatus::BadRequest, "bad_request", "Unsupported request target form");
            return;
        }
    }
    std::string_view raw_path = path_and_query;
    std::string_view raw_query;
    bool has_query = false;
    if (const auto q = path_and_query.find('?'); q != std::string_view::npos) {
        raw_path = path_and_query.substr(0, q);
        raw_query = path_and_query.substr(q + 1);
        has_query = true;
    }
    if (raw_path.empty()) raw_path = "/";
    if (!percent_decode_path(raw_path)) {
        fail(HttpStatus::BadRequest, "bad_request", "Malformed percent-encoding in path");
        return;
    }
    path_.assign(raw_path);
    if (has_query) {
        auto q = parse_query_string(raw_query);
        if (!q) {
            fail(HttpStatus::BadRequest, "bad_request", "Malformed percent-encoding in query string");
            return;
        }
        query_ = std::move(*q);
    }

    // --- Header fields ----------------------------------------------------------
    std::size_t header_count = 0;
    bool have_cl = false;
    std::string cl_raw;
    while (pos <= head.size()) {
        const std::string_view hline = next_line();
        if (++header_count > limits_.max_header_count) {
            fail(HttpStatus::RequestHeaderFieldsTooLarge, "header_too_large",
                 "Too many header fields");
            return;
        }
        if (hline.empty()) {
            fail(HttpStatus::BadRequest, "bad_request", "Empty header line");
            return;
        }
        if (hline.front() == ' ' || hline.front() == '\t') {
            fail(HttpStatus::BadRequest, "bad_request", "Obsolete header line folding is not allowed");
            return;
        }
        const auto colon = hline.find(':');
        if (colon == std::string_view::npos) {
            fail(HttpStatus::BadRequest, "bad_request", "Malformed header");
            return;
        }
        const std::string_view name = hline.substr(0, colon);
        if (!is_token(name)) {  // also rejects "Name : value" (space before colon)
            fail(HttpStatus::BadRequest, "bad_request", "Malformed header name");
            return;
        }
        const std::string_view value = trim_ows(hline.substr(colon + 1));
        if (!is_valid_field_value(value)) {
            fail(HttpStatus::BadRequest, "bad_request", "Invalid character in header value");
            return;
        }

        const std::string lname = to_lower(name);
        if (lname == "content-length") {
            if (have_cl && cl_raw != value) {
                fail(HttpStatus::BadRequest, "bad_request", "Conflicting Content-Length headers");
                return;
            }
            have_cl = true;
            cl_raw.assign(value);
            headers_[lname] = cl_raw;
        } else if (lname == "host") {
            if (headers_.count(lname) != 0) {
                fail(HttpStatus::BadRequest, "bad_request", "Duplicate Host header");
                return;
            }
            headers_[lname] = std::string(value);
        } else {
            auto [it, inserted] = headers_.try_emplace(lname, value);
            if (!inserted) {
                // RFC 7230 3.2.2: combine repeated fields with a comma
                // (Cookie uses "; " per RFC 6265).
                it->second += (lname == "cookie") ? "; " : ", ";
                it->second += value;
            }
        }
    }

    // --- Message framing --------------------------------------------------------
    if (headers_.count("transfer-encoding") != 0) {
        if (have_cl) {
            fail(HttpStatus::BadRequest, "bad_request",
                 "Content-Length and Transfer-Encoding must not both be present");
        } else {
            fail(HttpStatus::NotImplemented, "not_implemented",
                 "Transfer-Encoding is not supported; send a Content-Length body");
        }
        return;
    }
    if (have_cl) {
        const bool all_digits = !cl_raw.empty() && std::all_of(cl_raw.begin(), cl_raw.end(), [](char c) {
            return c >= '0' && c <= '9';
        });
        if (!all_digits) {
            fail(HttpStatus::BadRequest, "bad_request", "Invalid Content-Length");
            return;
        }
        std::size_t cl = 0;
        const auto [ptr, ec] = std::from_chars(cl_raw.data(), cl_raw.data() + cl_raw.size(), cl);
        if (ec == std::errc::result_out_of_range) {
            fail(HttpStatus::RequestEntityTooLarge, "body_too_large", "Request body too large");
            return;
        }
        if (ec != std::errc{} || ptr != cl_raw.data() + cl_raw.size()) {
            fail(HttpStatus::BadRequest, "bad_request", "Invalid Content-Length");
            return;
        }
        if (cl > limits_.max_body_size) {
            fail(HttpStatus::RequestEntityTooLarge, "body_too_large", "Request body too large");
            return;
        }
        content_length_ = cl;
    }

    if (const auto it = headers_.find("expect"); it != headers_.end() && version == Version::Http11) {
        if (to_lower(trim_ows(it->second)) == "100-continue") {
            want_continue_ = true;
        } else {
            fail(HttpStatus::ExpectationFailed, "expectation_failed", "Unsupported Expect value");
            return;
        }
    }

    if (content_length_ > 0) state_ = State::Body;
}

void RequestParser::finish() {
    result_ = ParseResult{};
    result_.request = Request(method_, std::move(path_), std::move(headers_), std::move(body_),
                              std::move(query_));
    result_.complete = true;
    result_.consumed = head_end_ + content_length_;
    state_ = State::Complete;
}

bool RequestParser::take_continue() noexcept {
    if (want_continue_ && state_ == State::Body) {
        want_continue_ = false;
        return true;
    }
    return false;
}

ParseResult RequestParser::take_result() {
    return std::move(result_);
}

ParseResult parse_http_request(std::string_view data, const ParserLimits& limits) {
    RequestParser parser(limits);
    parser.feed(data);
    if (parser.state() == RequestParser::State::Complete ||
        parser.state() == RequestParser::State::Failed) {
        return parser.take_result();
    }
    ParseResult incomplete;
    incomplete.complete = false;
    incomplete.consumed = data.size();
    return incomplete;
}

ParseResult parse_http_request(std::string_view data, std::size_t max_body_size) {
    ParserLimits limits;
    limits.max_body_size = max_body_size;
    return parse_http_request(data, limits);
}

} // namespace ourweb
