#pragma once

#include <chrono>
#include <cstddef>

namespace ourweb {

// Tunable limits and timeouts for the HTTP server. Every limit has a safe
// default; raise them deliberately, not by accident.
struct ServerOptions {
    // ---- Request limits (violations produce a 4xx response) ----------------

    // Maximum request body (Content-Length). Exceeding it yields 413.
    std::size_t max_body_size = 1024 * 1024;
    // Maximum size of the request line plus all header lines. Yields 431.
    std::size_t max_header_bytes = 16 * 1024;
    // Maximum length of the request-target (path + query). Yields 414.
    std::size_t max_target_bytes = 8 * 1024;
    // Maximum number of header fields. Yields 431.
    std::size_t max_header_count = 100;

    // ---- Connection limits --------------------------------------------------

    // Maximum simultaneously open connections. Additional connections are
    // answered with 503 and closed immediately.
    std::size_t max_connections = 1024;

    // ---- Timeouts -----------------------------------------------------------

    // Maximum time to wait for the next bytes of a request (slow-client guard).
    std::chrono::milliseconds read_timeout{15'000};
    // Maximum total time to receive one complete request.
    std::chrono::milliseconds request_timeout{30'000};
    // Maximum time a response write may make no progress.
    std::chrono::milliseconds write_timeout{15'000};
    // On shutdown, how long in-flight requests may keep running before their
    // sockets are forcibly shut down. Handlers themselves cannot be cancelled:
    // stop() always waits for running handlers to return.
    std::chrono::milliseconds shutdown_grace{5'000};
};

} // namespace ourweb
