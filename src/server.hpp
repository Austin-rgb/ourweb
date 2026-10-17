#pragma once

#include "ourweb/app.hpp"
#include "ourweb/server_options.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

#include "socket_io.hpp"

namespace ourweb {

// Internal blocking HTTP/1.1 server: one accept loop plus one worker thread per
// connection (bounded by ServerOptions::max_connections), one request per
// connection.
//
// Lifetime / shutdown contract
// ----------------------------
//  * Every worker thread is owned by the Server and JOINED before run()
//    returns; no thread is ever detached, so no worker can outlive the
//    Server or the App it references.
//  * stop() is thread-safe and idempotent. It wakes the accept loop, closes the
//    listening socket (no new connections), makes idle/partially-read
//    connections close promptly, and lets requests that are already being
//    handled finish. After ServerOptions::shutdown_grace the remaining sockets
//    are shut down to unblock stalled writes. Handler code cannot be
//    interrupted, so run() returns only after every running handler has
//    returned.
//  * A Server runs at most once.
class Server {
public:
    Server(const App& app, ServerOptions options);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Bind and serve until stop(). Throws std::runtime_error with a descriptive
    // message if the address is invalid or binding/listening fails.
    void run(std::string_view address);

    void stop() noexcept;

    // Block until the socket is listening (true), or run() failed/ended
    // (false), or the timeout expires (false).
    bool wait_until_listening(std::chrono::milliseconds timeout);

    [[nodiscard]] bool listening() const noexcept { return listening_.load(); }
    // The bound port (useful with port 0); 0 until listening.
    [[nodiscard]] std::uint16_t port() const noexcept { return port_.load(); }

private:
    struct Connection {
        std::thread thread;
        int fd = -1;        // guarded by mu_; closed by the worker thread
        bool done = false;  // guarded by mu_
    };

    void accept_loop(int listen_fd, int wake_fd, std::string& fatal);
    void accept_one(int listen_fd);
    void reap_finished();
    void drain_and_join();

    void serve_connection(Connection* conn) noexcept;
    void handle_connection(int fd);
    Response dispatch(Request request) noexcept;

    const App& app_;
    const ServerOptions options_;

    std::atomic<bool> stopping_{false};
    std::atomic<bool> listening_{false};
    std::atomic<std::uint16_t> port_{0};
    std::atomic<int> wake_write_fd_{-1};
    bool started_ = false;  // guarded by mu_
    bool finished_ = false;  // guarded by mu_

    io::UniqueFd wake_read_;
    io::UniqueFd wake_write_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::list<std::unique_ptr<Connection>> conns_;  // guarded by mu_
};

} // namespace ourweb
