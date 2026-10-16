#include "http_parser.hpp"

#include <cctype>
#include <charconv>
#include <sstream>
#include <unordered_map>

namespace ourweb {

namespace {

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool is_hex(char c) {
    return std::isxdigit(static_cast<unsigned char>(c));
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::optional<std::string> percent_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%') {
            if (i + 2 >= in.size() || !is_hex(in[i + 1]) || !is_hex(in[i + 2])) {
                return std::nullopt;
            }
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_query_string(std::string_view qs) {
    std::unordered_map<std::string, std::string> result;
    if (qs.empty()) return result;

    std::size_t start = 0;
    while (start < qs.size()) {
        auto amp = qs.find('&', start);
        auto part = (amp == std::string_view::npos) ? qs.substr(start) : qs.substr(start, amp - start);
        auto eq = part.find('=');
        std::string_view key_sv, val_sv;
        if (eq == std::string_view::npos) {
            key_sv = part;
            val_sv = "";
        } else {
            key_sv = part.substr(0, eq);
            val_sv = part.substr(eq + 1);
        }
        auto key = percent_decode(key_sv);
        auto val = percent_decode(val_sv);
        if (key && val) {
            result[*key] = *val;
        }
        if (amp == std::string_view::npos) break;
        start = amp + 1;
    }
    return result;
}

ParseResult parse_http_request(std::string_view data, std::size_t max_body_size) {
    ParseResult result;

    // Need at least the request line + CRLF
    auto header_end = data.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        // Incomplete
        result.complete = false;
        result.consumed = data.size();
        return result;
    }

    std::string_view headers_part = data.substr(0, header_end);
    std::size_t body_start = header_end + 4;

    // Parse request line
    auto first_crlf = headers_part.find("\r\n");
    if (first_crlf == std::string_view::npos) {
        result.error = true;
        result.error_message = "Malformed request line";
        return result;
    }

    std::string_view request_line = headers_part.substr(0, first_crlf);
    std::string_view header_block = headers_part.substr(first_crlf + 2);

    // method SP request-target SP HTTP-version
    auto sp1 = request_line.find(' ');
    if (sp1 == std::string_view::npos) {
        result.error = true;
        result.error_message = "Malformed request line";
        return result;
    }
    auto sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) {
        result.error = true;
        result.error_message = "Malformed request line";
        return result;
    }

    std::string_view method_sv = request_line.substr(0, sp1);
    std::string_view target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    // version ignored for now

    HttpMethod method = parse_method(method_sv);
    if (method == HttpMethod::Unknown) {
        result.error = true;
        result.error_message = "Unsupported HTTP method";
        return result;
    }

    // Split path and query
    std::string path;
    std::unordered_map<std::string, std::string> query;
    auto qpos = target.find('?');
    if (qpos == std::string_view::npos) {
        path = std::string(target);
    } else {
        path = std::string(target.substr(0, qpos));
        query = parse_query_string(target.substr(qpos + 1));
    }
    if (path.empty()) path = "/";

    // Parse headers
    std::unordered_map<std::string, std::string> headers;
    std::size_t pos = 0;
    while (pos < header_block.size()) {
        auto line_end = header_block.find("\r\n", pos);
        std::string_view line = (line_end == std::string_view::npos)
                                    ? header_block.substr(pos)
                                    : header_block.substr(pos, line_end - pos);
        if (line.empty()) break;

        auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            result.error = true;
            result.error_message = "Malformed header";
            return result;
        }
        std::string_view name = line.substr(0, colon);
        std::string_view value = line.substr(colon + 1);
        // Trim leading whitespace of value
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        headers[to_lower(name)] = std::string(value);

        if (line_end == std::string_view::npos) break;
        pos = line_end + 2;
    }

    // Content-Length
    std::size_t content_length = 0;
    auto cl_it = headers.find("content-length");
    if (cl_it != headers.end()) {
        auto [ptr, ec] = std::from_chars(cl_it->second.data(),
                                         cl_it->second.data() + cl_it->second.size(),
                                         content_length);
        if (ec != std::errc{}) {
            result.error = true;
            result.error_message = "Invalid Content-Length";
            return result;
        }
    }

    if (content_length > max_body_size) {
        result.error = true;
        result.error_message = "Request body too large";
        // Still mark as complete so the connection can respond with 413
        result.complete = true;
        result.consumed = body_start; // we don't need the body
        // Build a dummy request so caller can still answer
        result.request = Request(method, std::move(path), std::move(headers), "", std::move(query));
        return result;
    }

    if (data.size() < body_start + content_length) {
        // Incomplete body
        result.complete = false;
        result.consumed = data.size();
        return result;
    }

    std::string body(data.substr(body_start, content_length));
    result.consumed = body_start + content_length;
    result.complete = true;
    result.request = Request(method, std::move(path), std::move(headers), std::move(body), std::move(query));
    return result;
}

} // namespace ourweb
