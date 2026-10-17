#pragma once

// Internal: small POSIX socket helpers. Everything that needs to deal with
// partial reads/writes, EINTR/EAGAIN, stalled peers and disconnects lives here
// so the server logic stays readable.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string_view>

namespace ourweb::io {

using Clock = std::chrono::steady_clock;

// RAII owner of a file descriptor.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

enum class IoStatus {
    Ok,         // transferred (read: >0 bytes; write: everything)
    Closed,     // peer closed or reset the connection
    Timeout,    // deadline / stall timeout reached
    Cancelled,  // the cancel flag was raised
    Error       // unexpected socket error
};

struct ReadResult {
    IoStatus status = IoStatus::Error;
    std::size_t bytes = 0;
};

// Wait until `fd` is readable, `deadline` passes, or `*cancel` becomes true
// (polled every ~100 ms). `cancel` may be null.
ReadResult read_some(int fd, char* buf, std::size_t capacity, Clock::time_point deadline,
                     const std::atomic<bool>* cancel);

// Write ALL of `data`, looping over partial writes. `stall_timeout` bounds how
// long the peer may accept no bytes at all (it restarts after every successful
// partial write). Never raises SIGPIPE: a vanished client yields Closed.
IoStatus write_all(int fd, std::string_view data, std::chrono::milliseconds stall_timeout,
                   const std::atomic<bool>* cancel = nullptr);

// One non-blocking best-effort send (for tiny "go away" replies).
void send_best_effort(int fd, std::string_view data) noexcept;

// Half-close our side (so the peer sees EOF after the response) and then read
// and discard incoming bytes for a short while. This avoids closing a socket
// that still has unread request data, which would send a TCP RST and could make
// the client lose the response we just wrote (e.g. a 413 for an upload).
void drain_after_shutdown(int fd, std::chrono::milliseconds max_wait = std::chrono::milliseconds(500),
                          std::size_t max_bytes = 256 * 1024) noexcept;

// Per-connection socket options (SO_NOSIGPIPE where MSG_NOSIGNAL is missing).
void configure_client_socket(int fd) noexcept;

void set_cloexec(int fd) noexcept;
void set_nonblocking(int fd) noexcept;

} // namespace ourweb::io
