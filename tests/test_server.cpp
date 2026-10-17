// Integration tests: a real server on 127.0.0.1 (ephemeral port) driven by raw
// socket clients, so framing, limits, timeouts and shutdown are tested as a
// client would observe them.

#include <gtest/gtest.h>
#include <ourweb/app.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

using namespace ourweb;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

// ---- raw client helpers -------------------------------------------------------

int connect_to(std::uint16_t port, int rcvbuf = 0) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (rcvbuf > 0) ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// Read until EOF (or timeout). `pause` throttles the reader to force the
// server through partial writes.
std::string read_all(int fd, std::chrono::milliseconds timeout = 5s, std::chrono::microseconds pause = 0us,
                     std::size_t chunk = 65536) {
    std::string out;
    std::string buf(chunk, '\0');
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200'000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n > 0) {
            out.append(buf.data(), static_cast<std::size_t>(n));
            if (pause.count() > 0) std::this_thread::sleep_for(pause);
        } else if (n == 0) {
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
    }
    return out;
}

std::string http(std::uint16_t port, const std::string& raw, std::chrono::milliseconds timeout = 5s) {
    int fd = connect_to(port);
    if (fd < 0) return "<connect failed>";
    send_all(fd, raw);
    std::string r = read_all(fd, timeout);
    ::close(fd);
    return r;
}

int status_of(const std::string& resp) {
    if (resp.size() < 12 || resp.compare(0, 5, "HTTP/") != 0) return -1;
    return std::atoi(resp.c_str() + 9);
}
std::string body_of(const std::string& resp) {
    auto p = resp.find("\r\n\r\n");
    return p == std::string::npos ? "" : resp.substr(p + 4);
}
bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

std::string get(const std::string& path, const std::string& extra = "") {
    return "GET " + path + " HTTP/1.1\r\nHost: t\r\n" + extra + "\r\n";
}
std::string post_json(const std::string& path, const std::string& body) {
    return "POST " + path + " HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

// ---- server fixture -----------------------------------------------------------

struct Running {
    App app;
    std::thread thread;
    std::exception_ptr error;

    void start() {
        thread = std::thread([this] {
            try { app.run("127.0.0.1:0"); } catch (...) { error = std::current_exception(); }
        });
        if (!app.wait_until_ready(5s)) {
            app.stop();
            if (thread.joinable()) thread.join();
            ADD_FAILURE();
        }
    }
    std::uint16_t port() const { return app.local_port(); }
    void stop_and_join() {
        app.stop();
        if (thread.joinable()) thread.join();
    }
    ~Running() { stop_and_join(); }
};

struct CreateUser { std::string name; std::string email; };
void to_json(nlohmann::json& j, const CreateUser& u) { j = nlohmann::json{{"name", u.name}, {"email", u.email}}; }
void from_json(const nlohmann::json& j, CreateUser& u) { j.at("name").get_to(u.name); j.at("email").get_to(u.email); }

void install_routes(App& app) {
    app.get("/", [] { return text("Hello, world!"); });
    app.get("/users/{id}", [](Path<int> id) { return text("user " + std::to_string(*id)); });
    app.post("/users", [](Json<CreateUser> in) { return json(*in, HttpStatus::Created); });
    app.get("/boom", []() -> Response { throw std::runtime_error("internal secret"); });
    app.get("/empty", [] { return no_content(); });
    app.get("/big", [] {
        std::string body(4 * 1024 * 1024, '\0');
        for (std::size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>('a' + i % 23);
        return text(std::move(body));
    });
    app.put("/res", [] { return text("put"); });
}

std::string expected_big() {
    std::string body(4 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>('a' + i % 23);
    return body;
}

} // namespace

// ---------------------------------------------------------------------------
// Basic behaviour
// ---------------------------------------------------------------------------

TEST(Server, ServesGetAndReportsStandardErrors) {
    Running s;
    install_routes(s.app);
    s.start();
    const auto port = s.port();
    ASSERT_NE(port, 0);

    auto ok = http(port, get("/"));
    EXPECT_EQ(status_of(ok), 200);
    EXPECT_EQ(body_of(ok), "Hello, world!");
    EXPECT_TRUE(has(ok, "Content-Length: 13\r\n"));
    EXPECT_TRUE(has(ok, "Connection: close\r\n"));

    EXPECT_EQ(status_of(http(port, get("/nope"))), 404);
    auto not_allowed = http(port, "DELETE /res HTTP/1.1\r\nHost: t\r\n\r\n");
    EXPECT_EQ(status_of(not_allowed), 405);
    EXPECT_TRUE(has(not_allowed, "Allow: PUT\r\n"));
    EXPECT_EQ(status_of(http(port, get("/empty"))), 204);
}

TEST(Server, HeadReturnsHeadersWithoutBody) {
    Running s;
    install_routes(s.app);
    s.start();
    auto r = http(s.port(), "HEAD / HTTP/1.1\r\nHost: t\r\n\r\n");
    EXPECT_EQ(status_of(r), 200);
    EXPECT_TRUE(has(r, "Content-Length: 13\r\n"));
    EXPECT_EQ(body_of(r), "");
}

TEST(Server, PathParametersAndJson) {
    Running s;
    install_routes(s.app);
    s.start();
    const auto port = s.port();

    EXPECT_EQ(body_of(http(port, get("/users/42"))), "user 42");
    EXPECT_EQ(status_of(http(port, get("/users/abc"))), 400);

    auto created = http(port, post_json("/users", R"({"name":"Ann","email":"a@x.io"})"));
    EXPECT_EQ(status_of(created), 201);
    EXPECT_TRUE(has(created, "Content-Type: application/json"));
    EXPECT_TRUE(has(body_of(created), "\"name\":\"Ann\""));

    EXPECT_EQ(status_of(http(port, post_json("/users", "{oops"))), 400);
    EXPECT_EQ(status_of(http(port, "POST /users HTTP/1.1\r\nHost: t\r\nContent-Type: text/plain\r\n"
                                   "Content-Length: 2\r\n\r\n{}")),
              415);
}

TEST(Server, HandlerExceptionIsA500AndServerKeepsServing) {
    Running s;
    install_routes(s.app);
    s.start();
    auto r = http(s.port(), get("/boom"));
    EXPECT_EQ(status_of(r), 500);
    EXPECT_FALSE(has(r, "internal secret"));
    EXPECT_EQ(status_of(http(s.port(), get("/"))), 200);
}

TEST(Server, MiddlewareOrderObservedOnTheWire) {
    Running s;
    s.app.wrap([](const Request&, MiddlewareNext next) {
        Response r = next();
        r.header("X-Order", r.headers().count("X-Order") ? r.headers().at("X-Order") + ",A" : "A");
        return r;
    });
    s.app.wrap([](const Request&, MiddlewareNext next) {
        Response r = next();
        r.header("X-Order", r.headers().count("X-Order") ? r.headers().at("X-Order") + ",B" : "B");
        return r;
    });
    s.app.get("/", [] { return text("ok"); });
    s.start();
    // inner (B) finishes first, outer (A) last
    EXPECT_TRUE(has(http(s.port(), get("/")), "X-Order: B,A\r\n"));
}

// ---------------------------------------------------------------------------
// Request limits and malformed input over the wire
// ---------------------------------------------------------------------------

TEST(ServerLimits, OversizedBodyRejectedWith413AndClientCanReadIt) {
    Running s;
    install_routes(s.app);
    s.app.max_body_size(1024);
    s.start();
    // Client pushes a body well over the limit; the 413 must still arrive intact
    // (lingering close instead of an RST that would destroy the response).
    std::string body(200 * 1024, 'x');
    auto r = http(s.port(), post_json("/users", body));
    EXPECT_EQ(status_of(r), 413);
    EXPECT_TRUE(has(r, "body_too_large"));
}

TEST(ServerLimits, HeaderBlockTooLarge431) {
    Running s;
    install_routes(s.app);
    s.start();
    auto r = http(s.port(), get("/", "X-Big: " + std::string(40 * 1024, 'a') + "\r\n"));
    EXPECT_EQ(status_of(r), 431);
}

TEST(ServerLimits, MalformedAndAmbiguousRequestsRejected) {
    Running s;
    install_routes(s.app);
    s.start();
    const auto port = s.port();
    EXPECT_EQ(status_of(http(port, "GET / HTTP/1.1\nHost: t\n\n")), 400);
    EXPECT_EQ(status_of(http(port, "GET /a%zz HTTP/1.1\r\n\r\n")), 400);
    EXPECT_EQ(status_of(http(port, "GET / HTTP/2.0\r\n\r\n")), 505);
    EXPECT_EQ(status_of(http(port, "BREW / HTTP/1.1\r\n\r\n")), 501);
    EXPECT_EQ(status_of(http(port, "POST /users HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nabc")), 400);
    EXPECT_EQ(status_of(http(port, "POST /users HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")), 501);
    EXPECT_EQ(status_of(http(port, "POST /users HTTP/1.1\r\nContent-Length: 12abc\r\n\r\n")), 400);
    EXPECT_EQ(status_of(http(port, "GET / HTTP/1.1\r\nHost: t\r\n\r\n")), 200);  // still healthy
}

TEST(ServerLimits, Expect100ContinueIsHonoured) {
    Running s;
    install_routes(s.app);
    s.start();
    int fd = connect_to(s.port());
    ASSERT_TRUE(fd >= 0);
    const std::string body = R"({"name":"Ann","email":"a@x.io"})";
    ASSERT_TRUE(send_all(fd, "POST /users HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\n"
                             "Expect: 100-continue\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n"));
    char buf[64] = {};
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);   // interim response before any body is sent
    ASSERT_TRUE(n > 0);
    EXPECT_TRUE(std::string(buf, static_cast<std::size_t>(n)).rfind("HTTP/1.1 100 Continue", 0) == 0);
    ASSERT_TRUE(send_all(fd, body));
    auto rest = read_all(fd);
    ::close(fd);
    EXPECT_EQ(status_of(rest), 201);
}

TEST(ServerLimits, SlowClientGets408AndIdleClientIsJustClosed) {
    Running s;
    install_routes(s.app);
    ServerOptions o;
    o.read_timeout = 300ms;
    s.app.options(o);
    s.start();

    // Started a request, then stalled: 408.
    int fd = connect_to(s.port());
    ASSERT_TRUE(fd >= 0);
    send_all(fd, "GET / HT");
    auto r = read_all(fd, 3s);
    ::close(fd);
    EXPECT_EQ(status_of(r), 408);

    // Connected but never sent anything: closed silently, no bytes.
    int idle = connect_to(s.port());
    ASSERT_TRUE(idle >= 0);
    auto none = read_all(idle, 3s);
    ::close(idle);
    EXPECT_EQ(none.size(), 0u);

    EXPECT_EQ(status_of(http(s.port(), get("/"))), 200);
}

TEST(ServerLimits, ConnectionCapAnswers503) {
    Running s;
    install_routes(s.app);
    ServerOptions o;
    o.max_connections = 1;
    s.app.options(o);
    s.start();

    int holder = connect_to(s.port());   // occupies the only slot (idle)
    ASSERT_TRUE(holder >= 0);
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(status_of(http(s.port(), get("/"))), 503);
    ::close(holder);

    // Slot is released once the holder is gone.
    int status = 0;
    for (int i = 0; i < 40 && status != 200; ++i) {
        std::this_thread::sleep_for(100ms);
        status = status_of(http(s.port(), get("/")));
    }
    EXPECT_EQ(status, 200);
}

// ---------------------------------------------------------------------------
// Partial writes and disconnected clients
// ---------------------------------------------------------------------------

TEST(ServerIo, LargeResponseSurvivesPartialWritesToASlowReader) {
    Running s;
    install_routes(s.app);
    s.start();
    // Tiny receive buffer + throttled reads force many partial send() calls.
    int fd = connect_to(s.port(), 4096);
    ASSERT_TRUE(fd >= 0);
    ASSERT_TRUE(send_all(fd, get("/big")));
    std::string r = read_all(fd, 30s, 100us, 16384);
    ::close(fd);
    ASSERT_EQ(status_of(r), 200);
    const std::string body = body_of(r);
    EXPECT_EQ(body.size(), 4u * 1024 * 1024);
    EXPECT_TRUE(body == expected_big());   // byte-exact: nothing dropped or duplicated
}

TEST(ServerIo, ClientsThatVanishMidResponseDoNotKillTheServer) {
    Running s;
    install_routes(s.app);
    s.start();
    for (int i = 0; i < 30; ++i) {
        int fd = connect_to(s.port(), 4096);
        ASSERT_TRUE(fd >= 0);
        send_all(fd, get("/big"));
        switch (i % 3) {
            case 0: {
                // Take a sip, then reset (RST): the server's next send fails with ECONNRESET.
                char tmp[1024];
                (void)::recv(fd, tmp, sizeof(tmp), 0);
                linger lg{1, 0};
                ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
                break;
            }
            case 1:
                // Close immediately with an EMPTY receive queue: a clean FIN. The
                // server's first segments are answered with RST and the send AFTER
                // that fails with EPIPE, which raises SIGPIPE (killing the whole
                // process) unless the library suppresses it.
                break;
            default: {
                char tmp[1024];  // sip, then close with unread data (implicit RST)
                (void)::recv(fd, tmp, sizeof(tmp), 0);
                break;
            }
        }
        ::close(fd);
    }
    // And clients that disconnect before sending a full request.
    for (int i = 0; i < 10; ++i) {
        int fd = connect_to(s.port());
        ASSERT_TRUE(fd >= 0);
        send_all(fd, "POST /users HTTP/1.1\r\nContent-Length: 100\r\n\r\nabc");
        ::close(fd);
    }
    // Reaching this line means no SIGPIPE killed the process. Server still works:
    EXPECT_EQ(status_of(http(s.port(), get("/"))), 200);
    EXPECT_TRUE(s.app.is_running());
}

TEST(ServerIo, ManyConcurrentClients) {
    Running s;
    install_routes(s.app);
    s.start();
    std::atomic<int> ok{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < 32; ++i) {
        clients.emplace_back([&, i] {
            for (int k = 0; k < 10; ++k) {
                auto r = http(s.port(), get("/users/" + std::to_string(i)));
                if (status_of(r) == 200 && body_of(r) == "user " + std::to_string(i)) ++ok;
            }
        });
    }
    for (auto& t : clients) t.join();
    EXPECT_EQ(ok.load(), 320);
}

// ---------------------------------------------------------------------------
// Startup failures and lifecycle
// ---------------------------------------------------------------------------

TEST(ServerLifecycle, InvalidAddressesProduceUnderstandableErrors) {
    for (const char* bad : {"nonsense", "127.0.0.1", "127.0.0.1:", "127.0.0.1:99999", "127.0.0.1:abc",
                            "999.1.1.1:80", "::1:80", "[::1]80"}) {
        App app;
        try {
            app.run(bad);
            ADD_FAILURE();
        } catch (const std::runtime_error& e) {
            EXPECT_TRUE(std::string(e.what()).size() > 10);
        }
        EXPECT_FALSE(app.is_running());
    }
}

TEST(ServerLifecycle, BindFailureNamesTheAddress) {
    Running first;
    install_routes(first.app);
    first.start();
    App second;
    const std::string addr = "127.0.0.1:" + std::to_string(first.port());
    try {
        second.run(addr);
        ADD_FAILURE();
    } catch (const std::runtime_error& e) {
        EXPECT_TRUE(has(e.what(), addr));
        EXPECT_TRUE(has(e.what(), "bind"));
    }
    EXPECT_FALSE(second.is_running());
}

TEST(ServerLifecycle, StopWakesTheAcceptLoopPromptly) {
    Running s;
    install_routes(s.app);
    s.start();
    EXPECT_EQ(status_of(http(s.port(), get("/"))), 200);
    const auto t0 = Clock::now();
    s.stop_and_join();
    EXPECT_LT(Clock::now() - t0, 400ms);   // the old code blocked in accept() forever; without the wake pipe this would take ~500 ms (poll interval)
    EXPECT_FALSE(s.app.is_running());
    EXPECT_EQ(s.error, nullptr);
}

TEST(ServerLifecycle, StopClosesIdleConnectionsAndStopsAccepting) {
    Running s;
    install_routes(s.app);
    s.start();
    const auto port = s.port();

    int idle1 = connect_to(port);
    int idle2 = connect_to(port);
    int partial = connect_to(port);
    ASSERT_TRUE(idle1 >= 0 && idle2 >= 0 && partial >= 0);
    send_all(partial, "GET / HTT");
    std::this_thread::sleep_for(150ms);

    const auto t0 = Clock::now();
    s.stop_and_join();   // must not hang on connections that are waiting for input
    EXPECT_LT(Clock::now() - t0, 2s);

    // The connections were closed by the server (EOF), not left dangling.
    EXPECT_EQ(read_all(idle1, 2s).size(), 0u);
    EXPECT_EQ(read_all(partial, 2s).size(), 0u);
    ::close(idle1); ::close(idle2); ::close(partial);

    // And nobody is accepting any more.
    EXPECT_TRUE(connect_to(port) < 0);
}

TEST(ServerLifecycle, InFlightRequestFinishesDuringShutdown) {
    Running s;
    std::atomic<bool> started{false};
    s.app.get("/slow", [&started] {
        started = true;
        std::this_thread::sleep_for(600ms);
        return text("finished");
    });
    s.start();

    std::string result;
    std::thread client([&] { result = http(s.port(), get("/slow"), 10s); });
    while (!started) std::this_thread::sleep_for(5ms);

    s.stop_and_join();      // returns only after the handler returned and its response was written
    client.join();
    EXPECT_EQ(status_of(result), 200);
    EXPECT_EQ(body_of(result), "finished");
}

TEST(ServerLifecycle, StalledWriterIsForcedClosedAfterGrace) {
    Running s;
    install_routes(s.app);
    ServerOptions o;
    o.shutdown_grace = 300ms;
    o.write_timeout = 60s;   // only the forced shutdown can free this worker
    s.app.options(o);
    s.start();

    int fd = connect_to(s.port(), 2048);
    ASSERT_TRUE(fd >= 0);
    send_all(fd, get("/big"));       // ...and never read the 4 MiB reply
    std::this_thread::sleep_for(300ms);

    const auto t0 = Clock::now();
    s.stop_and_join();
    EXPECT_LT(Clock::now() - t0, 3s);
    ::close(fd);
}

TEST(ServerLifecycle, StopWhenNotRunningIsHarmlessAndAppIsReusable) {
    App app;
    install_routes(app);
    app.stop();
    app.stop();
    for (int round = 0; round < 3; ++round) {
        std::thread t([&] { app.run("127.0.0.1:0"); });
        ASSERT_TRUE(app.wait_until_ready(5s));
        EXPECT_EQ(status_of(http(app.local_port(), get("/"))), 200);
        app.stop();
        app.stop();   // idempotent
        t.join();
        EXPECT_FALSE(app.is_running());
        EXPECT_EQ(app.local_port(), 0);
    }
}

TEST(ServerLifecycle, StopFromInsideAHandler) {
    Running s;
    s.app.get("/quit", [&s] {
        s.app.stop();
        return text("bye");
    });
    s.start();
    auto r = http(s.port(), get("/quit"));
    EXPECT_EQ(body_of(r), "bye");
    s.thread.join();   // run() returns on its own
    EXPECT_FALSE(s.app.is_running());
}

TEST(ServerLifecycle, DestroyingARunningAppStopsAndWaitsForRun) {
    // The old design let detached workers outlive the server/app (use-after-free).
    std::atomic<bool> run_returned{false};
    std::thread t;
    {
        auto app = std::make_unique<App>();
        install_routes(*app);
        App* raw = app.get();
        t = std::thread([raw, &run_returned] {
            raw->run("127.0.0.1:0");
            run_returned = true;
        });
        ASSERT_TRUE(raw->wait_until_ready(5s));
        int idle = connect_to(raw->local_port());   // a live connection at destruction time
        std::this_thread::sleep_for(100ms);
        app.reset();                                 // ~App: stop + wait
        EXPECT_TRUE(idle >= 0);
        if (idle >= 0) ::close(idle);
    }
    t.join();
    EXPECT_TRUE(run_returned.load());
}

TEST(ServerLifecycle, ConfigurationIsFrozenWhileRunning) {
    Running s;
    install_routes(s.app);
    s.start();
    EXPECT_THROW(s.app.get("/late", [] { return text("x"); }), std::logic_error);
    EXPECT_THROW(s.app.wrap([](const Request&, MiddlewareNext n) { return n(); }), std::logic_error);
    EXPECT_THROW(s.app.state(1), std::logic_error);
    EXPECT_THROW(s.app.max_body_size(1), std::logic_error);
    EXPECT_THROW(s.app.run("127.0.0.1:0"), std::logic_error);   // already running
    EXPECT_EQ(status_of(http(s.port(), get("/"))), 200);
}
