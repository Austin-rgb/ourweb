#include "socket_io.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace ourweb::io {

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL | MSG_DONTWAIT;
#else
constexpr int kSendFlags = MSG_DONTWAIT;  // SO_NOSIGPIPE is set per socket instead
#endif

enum class Wait { Ready, Timeout, Cancelled, Error };

Wait wait_for(int fd, short events, Clock::time_point deadline, const std::atomic<bool>* cancel) {
    using namespace std::chrono;
    constexpr milliseconds slice{100};
    for (;;) {
        if (cancel && cancel->load(std::memory_order_acquire)) return Wait::Cancelled;
        const auto now = Clock::now();
        if (now >= deadline) return Wait::Timeout;
        const auto remaining = duration_cast<milliseconds>(deadline - now) + milliseconds(1);
        const int timeout_ms = static_cast<int>(std::min(remaining, slice).count());

        pollfd p{};
        p.fd = fd;
        p.events = events;
        const int rc = ::poll(&p, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return Wait::Error;
        }
        if (rc == 0) continue;
        if (p.revents & (POLLERR | POLLNVAL)) return Wait::Error;
        if (p.revents & events) return Wait::Ready;
        if (p.revents & POLLHUP) {
            // For reads, let recv() report EOF / buffered data; for writes the
            // peer is gone.
            return (events & POLLIN) ? Wait::Ready : Wait::Error;
        }
    }
}

bool is_disconnect_errno(int e) {
    return e == ECONNRESET || e == EPIPE || e == ECONNABORTED || e == ENOTCONN || e == ETIMEDOUT;
}

} // namespace

void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
}

ReadResult read_some(int fd, char* buf, std::size_t capacity, Clock::time_point deadline,
                     const std::atomic<bool>* cancel) {
    for (;;) {
        switch (wait_for(fd, POLLIN, deadline, cancel)) {
            case Wait::Ready: break;
            case Wait::Timeout: return {IoStatus::Timeout, 0};
            case Wait::Cancelled: return {IoStatus::Cancelled, 0};
            case Wait::Error: return {IoStatus::Error, 0};
        }
        const ssize_t n = ::recv(fd, buf, capacity, MSG_DONTWAIT);
        if (n > 0) return {IoStatus::Ok, static_cast<std::size_t>(n)};
        if (n == 0) return {IoStatus::Closed, 0};
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return {is_disconnect_errno(errno) ? IoStatus::Closed : IoStatus::Error, 0};
    }
}

IoStatus write_all(int fd, std::string_view data, std::chrono::milliseconds stall_timeout,
                   const std::atomic<bool>* cancel) {
    auto stall_deadline = Clock::now() + stall_timeout;
    while (!data.empty()) {
        const ssize_t n = ::send(fd, data.data(), data.size(), kSendFlags);
        if (n > 0) {
            data.remove_prefix(static_cast<std::size_t>(n));
            stall_deadline = Clock::now() + stall_timeout;  // progress made
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            switch (wait_for(fd, POLLOUT, stall_deadline, cancel)) {
                case Wait::Ready: continue;
                case Wait::Timeout: return IoStatus::Timeout;
                case Wait::Cancelled: return IoStatus::Cancelled;
                case Wait::Error: return IoStatus::Closed;
            }
        }
        return (n < 0 && is_disconnect_errno(errno)) ? IoStatus::Closed : IoStatus::Error;
    }
    return IoStatus::Ok;
}

void send_best_effort(int fd, std::string_view data) noexcept {
    (void)::send(fd, data.data(), data.size(), kSendFlags);
}

void drain_after_shutdown(int fd, std::chrono::milliseconds max_wait, std::size_t max_bytes) noexcept {
    ::shutdown(fd, SHUT_WR);
    const auto deadline = Clock::now() + max_wait;
    char sink[4096];
    std::size_t drained = 0;
    while (drained < max_bytes) {
        const ReadResult r = read_some(fd, sink, sizeof(sink), deadline, nullptr);
        if (r.status != IoStatus::Ok) break;
        drained += r.bytes;
    }
}

void configure_client_socket(int fd) noexcept {
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

void set_cloexec(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void set_nonblocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

} // namespace ourweb::io
