#include "url.hpp"

namespace ourweb {

namespace {

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<std::string> decode(std::string_view in, bool plus_is_space) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '\0') return std::nullopt;
        if (c == '%') {
            if (in.size() - i < 3) return std::nullopt;
            const int hi = hex_val(in[i + 1]);
            const int lo = hex_val(in[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            const auto byte = static_cast<unsigned char>((hi << 4) | lo);
            if (byte == 0) return std::nullopt;
            out.push_back(static_cast<char>(byte));
            i += 2;
        } else if (c == '+' && plus_is_space) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

std::optional<std::string> percent_decode(std::string_view in) {
    return decode(in, /*plus_is_space=*/true);
}

std::optional<std::string> percent_decode_path(std::string_view in) {
    return decode(in, /*plus_is_space=*/false);
}

std::optional<QueryMap> parse_query_string(std::string_view qs) {
    QueryMap result;
    std::size_t start = 0;
    while (start <= qs.size() && !qs.empty()) {
        const auto amp = qs.find('&', start);
        const auto part = (amp == std::string_view::npos) ? qs.substr(start)
                                                           : qs.substr(start, amp - start);
        if (!part.empty()) {
            const auto eq = part.find('=');
            const auto key_sv = (eq == std::string_view::npos) ? part : part.substr(0, eq);
            const auto val_sv = (eq == std::string_view::npos) ? std::string_view{}
                                                               : part.substr(eq + 1);
            auto key = percent_decode(key_sv);
            auto val = percent_decode(val_sv);
            if (!key || !val) return std::nullopt;
            result[std::move(*key)] = std::move(*val);
        }
        if (amp == std::string_view::npos) break;
        start = amp + 1;
    }
    return result;
}

} // namespace ourweb
