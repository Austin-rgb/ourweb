#include "server.hpp"

#include "http_parser.hpp"
#include "ourweb/error.hpp"
#include "ourweb/http.hpp"
#include "ourweb/response.hpp"
#include "response_writer.hpp"
#include "socket_io.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace ourweb {

namespace {

using std::chrono::milliseconds;

void log_error(const std::string& message) {
    std::cerr << "[ourweb] " << message << '\n';
}

struct Endpoint {
    sockaddr_storage addr{};
    socklen_t len = 0;
    int family = AF_INET;
    std::string host;
};

// Accepts "host:port", "[ipv6]:port", "localhost:port", "0.0.0.0:port"; port 0
// picks an ephemeral port.
Endpoint parse_address(std::string_view address) {
    std::string host;
    std::string_view port_sv;
    if (!address.empty() && address.front() == '[') {
        const auto close = address.find(']');
        if (close == std::string_view::npos || close + 1 >= address.size() ||
            address[close + 1] != ':') {
            throw std::runtime_error("Invalid address \"" + std::string(address) +
                                     "\", expected [ipv6]:port");
        }
        host = std::string(address.substr(1, close - 1));
        port_sv = address.substr(close + 2);
    } else {
        const auto colon = address.rfind(':');
        if (colon == std::string_view::npos) {
            throw std::runtime_error("Invalid address \"" + std::string(address) +
                                     "\", expected host:port");
        }
        host = std::string(address.substr(0, colon));
        port_sv = address.substr(colon + 1);
        if (host.find(':') != std::string::npos) {
            throw std::runtime_error("IPv6 addresses must be written as [addr]:port");
        }
    }

    unsigned short port = 0;
    {
        const auto [ptr, ec] = std::from_chars(port_sv.data(), port_sv.data() + port_sv.size(), port);
        if (port_sv.empty() || ec != std::errc{} || ptr != port_sv.data() + port_sv.size()) {
            throw std::runtime_error("Invalid port number in \"" + std::string(address) + "\"");
        }
    }

    Endpoint ep;
    ep.host = host;
    const bool v6 = address.front() == '[';
    if (v6) {
        auto* a = reinterpret_cast<sockaddr_in6*>(&ep.addr);
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(port);
        if (::inet_pton(AF_INET6, host.c_str(), &a->sin6_addr) != 1) {
            throw std::runtime_error("Invalid IPv6 address: " + host);
        }
        ep.family = AF_INET6;
        ep.len = sizeof(sockaddr_in6);
    } else {
        auto* a = reinterpret_cast<sockaddr_in*>(&ep.addr);
        a->sin_family = AF_INET;
        a->sin_port = htons(port);
        if (host == "0.0.0.0" || host == "*") {
            a->sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (host == "localhost") {
            a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        } else if (::inet_pton(AF_INET, host.c_str(), &a->sin_addr) != 1) {
            throw std::runtime_error("Invalid IP address: " + host);
        }
        ep.family = AF_INET;
        ep.len = sizeof(sockaddr_in);
    }
    return ep;
}

std::string errno_message() {
    return std::system_category().message(errno);
}

io::UniqueFd open_listener(const Endpoint& ep, const std::string& address, std::uint16_t& bound_port) {
    io::UniqueFd fd(::socket(ep.family, SOCK_STREAM, 0));
    if (!fd.valid()) {
        throw std::runtime_error("Failed to create socket: " + errno_message());
    }
    io::set_cloexec(fd.get());
    const int yes = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&ep.addr), ep.len) < 0) {
        throw std::runtime_error("Failed to bind to " + address + ": " + errno_message());
    }
    if (::listen(fd.get(), 128) < 0) {
        throw std::runtime_error("Failed to listen on " + address + ": " + errno_message());
    }

    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        bound_port = ntohs(bound.ss_family == AF_INET6
                               ? reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port
                               : reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
    }
    return fd;
}

int accept_connection(int listen_fd) {
#ifdef __linux__
    return ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
#else
    const int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd >= 0) io::set_cloexec(fd);
    return fd;
#endif
}

} // namespace

Server::Server(const App& app, ServerOptions options) : app_(app), options_(options) {}

Server::~Server() {
    stop();
    // run() joins its workers itself; this only matters if run() threw midway.
    std::list<std::unique_ptr<Connection>> leftovers;
    {
        std::lock_guard<std::mutex> lk(mu_);
        leftovers = std::move(conns_);
    }
    for (auto& c : leftovers) {
        if (c->thread.joinable()) c->thread.join();
    }
}

void Server::stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    const int fd = wake_write_fd_.load(std::memory_order_acquire);
    if (fd >= 0) {
        const char byte = 1;
        const ssize_t n = ::write(fd, &byte, 1);  // async-signal-safe
        (void)n;  // a full pipe already means a wake-up is pending
    }
    cv_.notify_all();
}

bool Server::wait_until_listening(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, timeout, [this] { return listening_.load() || finished_; });
    return listening_.load();
}

void Server::run(std::string_view address) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (started_) throw std::logic_error("ourweb: a Server can only be run once");
        started_ = true;
    }

    // Always mark the run as finished (and wake waiters) however we leave.
    struct FinishGuard {
        Server& s;
        ~FinishGuard() {
            {
                std::lock_guard<std::mutex> lk(s.mu_);
                s.finished_ = true;
            }
            s.listening_.store(false);
            s.cv_.notify_all();
        }
    } finish_guard{*this};

    const std::string address_str(address);
    const Endpoint ep = parse_address(address);

    std::uint16_t bound_port = 0;
    io::UniqueFd listener = open_listener(ep, address_str, bound_port);

    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
        throw std::runtime_error("Failed to create wake pipe: " + errno_message());
    }
    wake_read_.reset(pipe_fds[0]);
    wake_write_.reset(pipe_fds[1]);
    io::set_cloexec(pipe_fds[0]);
    io::set_cloexec(pipe_fds[1]);
    io::set_nonblocking(pipe_fds[1]);
    // Publish the wake fd BEFORE checking stopping_, so a concurrent stop()
    // either sees the fd (and wakes us) or is seen by the check below.
    wake_write_fd_.store(pipe_fds[1], std::memory_order_release);

    port_.store(bound_port);
    {
        std::lock_guard<std::mutex> lk(mu_);
        listening_.store(true);
    }
    cv_.notify_all();
    std::cout << "[ourweb] listening on " << (ep.family == AF_INET6 ? "[" + ep.host + "]" : ep.host)
              << ':' << bound_port << std::endl;

    std::string fatal;
    accept_loop(listener.get(), wake_read_.get(), fatal);

    // Shutdown: stop accepting, let in-flight requests finish, join workers.
    listening_.store(false);
    listener.reset();
    drain_and_join();
    wake_write_fd_.store(-1, std::memory_order_release);

    if (!fatal.empty()) throw std::runtime_error(fatal);
}

void Server::accept_loop(int listen_fd, int wake_fd, std::string& fatal) {
    while (!stopping_.load(std::memory_order_acquire)) {
        std::array<pollfd, 2> fds{};
        fds[0].fd = listen_fd;
        fds[0].events = POLLIN;
        fds[1].fd = wake_fd;
        fds[1].events = POLLIN;

        const int rc = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 500);
        reap_finished();
        if (rc < 0) {
            if (errno == EINTR) continue;
            fatal = "poll() on the listening socket failed: " + errno_message();
            return;
        }
        if (rc == 0) continue;
        if (fds[1].revents & POLLIN) return;  // stop() requested
        if (fds[0].revents & (POLLERR | POLLNVAL)) {
            fatal = "listening socket failed";
            return;
        }
        if (fds[0].revents & POLLIN) accept_one(listen_fd);
    }
}

void Server::accept_one(int listen_fd) {
    const int cfd = accept_connection(listen_fd);
    if (cfd < 0) {
        switch (errno) {
            case EINTR: case EAGAIN: case ECONNABORTED: case EPROTO:
                return;  // transient; the client went away before we accepted
            case EMFILE: case ENFILE: case ENOBUFS: case ENOMEM:
                // Out of descriptors/memory: back off instead of spinning at
                // 100% CPU retrying accept() that will keep failing.
                log_error("accept() failed (" + errno_message() + "); backing off");
                std::this_thread::sleep_for(milliseconds(100));
                return;
            default:
                log_error("accept() failed: " + errno_message());
                std::this_thread::sleep_for(milliseconds(10));
                return;
        }
    }
    io::configure_client_socket(cfd);

    reap_finished();
    auto conn = std::make_unique<Connection>();
    conn->fd = cfd;
    Connection* raw = conn.get();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (conns_.size() >= options_.max_connections) {
            io::send_best_effort(
                cfd, serialize_response(error_response(HttpStatus::ServiceUnavailable, "server_busy",
                                                       "Server is at capacity, try again later"),
                                        false));
            ::close(cfd);
            return;
        }
        conns_.push_back(std::move(conn));
    }

    try {
        raw->thread = std::thread([this, raw] { serve_connection(raw); });
    } catch (const std::system_error& e) {
        log_error(std::string("cannot start connection thread: ") + e.what());
        std::lock_guard<std::mutex> lk(mu_);
        ::close(cfd);
        conns_.remove_if([raw](const std::unique_ptr<Connection>& c) { return c.get() == raw; });
    }
}

void Server::reap_finished() {
    std::vector<std::unique_ptr<Connection>> finished;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = conns_.begin(); it != conns_.end();) {
            if ((*it)->done) {
                finished.push_back(std::move(*it));
                it = conns_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Joined outside the lock; workers touch mu_ only before marking done.
    for (auto& c : finished) {
        if (c->thread.joinable()) c->thread.join();
    }
}

void Server::drain_and_join() {
    std::list<std::unique_ptr<Connection>> conns;
    {
        std::unique_lock<std::mutex> lk(mu_);
        auto all_done = [this] {
            return std::all_of(conns_.begin(), conns_.end(),
                               [](const std::unique_ptr<Connection>& c) { return c->done; });
        };
        // Idle and half-read connections notice stopping_ within ~100 ms and
        // close; requests already in a handler get the grace period to finish.
        if (!cv_.wait_for(lk, options_.shutdown_grace, all_done)) {
            for (auto& c : conns_) {
                // fd is only valid while !done, and the worker closes it under
                // mu_, so this cannot hit a recycled descriptor.
                if (!c->done && c->fd >= 0) ::shutdown(c->fd, SHUT_RDWR);
            }
        }
        conns = std::move(conns_);
        conns_.clear();
    }
    // Workers still need mu_ to finish, so join without holding it.
    for (auto& c : conns) {
        if (c->thread.joinable()) c->thread.join();
    }
}

void Server::serve_connection(Connection* conn) noexcept {
    const int fd = conn->fd;  // fixed before the thread was started
    try {
        handle_connection(fd);
    } catch (const std::exception& e) {
        log_error(std::string("connection error: ") + e.what());
    } catch (...) {
        log_error("connection error: unknown exception");
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        ::close(fd);
        conn->fd = -1;
        conn->done = true;
    }
    cv_.notify_all();
}

Response Server::dispatch(Request request) noexcept {
    try {
        return app_.handle_request(std::move(request));
    } catch (const std::exception& e) {
        log_error(std::string("uncaught exception: ") + e.what());
    } catch (...) {
        log_error("uncaught non-standard exception");
    }
    return internal_error();
}

void Server::handle_connection(int fd) {
    ParserLimits limits;
    limits.max_body_size = options_.max_body_size;
    limits.max_header_bytes = options_.max_header_bytes;
    limits.max_target_bytes = options_.max_target_bytes;
    limits.max_header_count = options_.max_header_count;
    RequestParser parser(limits);

    const auto request_deadline = io::Clock::now() + options_.request_timeout;
    std::array<char, 8192> buf;

    while (parser.state() == RequestParser::State::Head ||
           parser.state() == RequestParser::State::Body) {
        const auto deadline = std::min(io::Clock::now() + options_.read_timeout, request_deadline);
        const io::ReadResult rr = io::read_some(fd, buf.data(), buf.size(), deadline, &stopping_);
        switch (rr.status) {
            case io::IoStatus::Ok:
                parser.feed(std::string_view(buf.data(), rr.bytes));
                if (parser.take_continue()) {
                    // The client is waiting for permission to send the body.
                    if (io::write_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", options_.write_timeout,
                                      &stopping_) != io::IoStatus::Ok) {
                        return;
                    }
                }
                break;
            case io::IoStatus::Timeout: {
                // Only answer clients that actually started a request; an idle
                // connection that sent nothing is just closed.
                if (parser.received_any()) {
                    const auto wire = serialize_response(
                        error_response(HttpStatus::RequestTimeout, "request_timeout",
                                       "Timed out waiting for the request"),
                        false);
                    if (io::write_all(fd, wire, options_.write_timeout) == io::IoStatus::Ok) {
                        io::drain_after_shutdown(fd);
                    }
                }
                return;
            }
            case io::IoStatus::Closed:     // client disconnected mid-request
            case io::IoStatus::Cancelled:  // server is shutting down
            case io::IoStatus::Error:
                return;
        }
    }

    ParseResult parsed = parser.take_result();
    Response response;
    bool is_head = false;
    if (parsed.error) {
        response = error_response(parsed.error_status, parsed.error_code, parsed.error_message);
    } else {
        is_head = parsed.request.method() == HttpMethod::Head;
        response = dispatch(std::move(parsed.request));
    }

    const std::string wire = serialize_response(response, is_head);
    // No cancel flag: a response already in flight is allowed to finish during
    // shutdown (bounded by the write stall timeout and, finally, the forced
    // socket shutdown after the grace period).
    const io::IoStatus st = io::write_all(fd, wire, options_.write_timeout);
    if (st == io::IoStatus::Ok) {
        io::drain_after_shutdown(fd);
    }
    // Closed/Timeout/Error: the client is gone or stalled; nothing more to do.
}

} // namespace ourweb
