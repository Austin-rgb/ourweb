#include "server.hpp"
#include "http_parser.hpp"
#include "ourweb/http.hpp"
#include "ourweb/response.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <charconv>

namespace ourweb {

namespace {

std::string serialize_response(const Response& resp, bool is_head) {
    std::ostringstream oss;
    auto status = resp.status_code();
    oss << "HTTP/1.1 " << static_cast<int>(status) << ' ' << status_reason(status) << "\r\n";

    bool has_content_length = false;
    bool has_connection = false;
    for (const auto& [k, v] : resp.headers()) {
        oss << k << ": " << v << "\r\n";
        std::string lower = k;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "content-length") has_content_length = true;
        if (lower == "connection") has_connection = true;
    }

    if (!has_content_length) {
        oss << "Content-Length: " << resp.body().size() << "\r\n";
    }
    if (!has_connection) {
        oss << "Connection: close\r\n";
    }
    oss << "\r\n";
    if (!is_head) {
        oss << resp.body();
    }
    return oss.str();
}

void handle_client(int client_fd, App& app) {
    std::string data;
    char buf[8192];
    while (true) {
        ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        data.append(buf, static_cast<std::size_t>(n));

        auto result = parse_http_request(data, app.max_body_size());
        if (result.error) {
            Response err = (result.error_message == "Request body too large")
                               ? body_too_large()
                               : bad_request(result.error_message);
            auto msg = serialize_response(err, false);
            ::send(client_fd, msg.data(), msg.size(), 0);
            break;
        }
        if (!result.complete) {
            continue; // need more data
        }

        bool is_head = (result.request.method() == HttpMethod::Head);
        Response resp;
        try {
            resp = app.handle_request(std::move(result.request));
        } catch (const std::exception& e) {
            std::cerr << "[ourweb] uncaught exception: " << e.what() << '\n';
            resp = internal_error();
        } catch (...) {
            resp = internal_error();
        }

        auto msg = serialize_response(resp, is_head);
        ::send(client_fd, msg.data(), msg.size(), 0);
        break; // one request per connection for simplicity
    }
    ::close(client_fd);
}

} // namespace

Server::Server(App& app) : app_(app) {}

Server::~Server() {
    stop();
}

void Server::run(std::string_view address) {
    auto colon = address.rfind(':');
    if (colon == std::string_view::npos) {
        throw std::runtime_error("Invalid address format, expected host:port");
    }
    std::string host(address.substr(0, colon));
    std::string port_str(address.substr(colon + 1));
    unsigned short port = 0;
    {
        auto [ptr, ec] = std::from_chars(port_str.data(),
                                         port_str.data() + port_str.size(), port);
        if (ec != std::errc{} || ptr != port_str.data() + port_str.size()) {
            throw std::runtime_error("Invalid port number");
        }
    }

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        throw std::runtime_error("Failed to create socket");
    }

    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host == "0.0.0.0" || host == "*") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(listen_fd);
        throw std::runtime_error("Invalid IP address: " + host);
    }

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd);
        throw std::runtime_error("Failed to bind to " + std::string(address));
    }
    if (::listen(listen_fd, 128) < 0) {
        ::close(listen_fd);
        throw std::runtime_error("Failed to listen");
    }

    running_ = true;
    std::cout << "[ourweb] listening on " << address << '\n';

    // Simple accept loop (single-threaded accept; per-connection threads)
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            if (!running_) break;
            continue;
        }
        std::thread([client_fd, this] {
            handle_client(client_fd, app_);
        }).detach();
    }

    ::close(listen_fd);
}

void Server::stop() {
    running_ = false;
}

} // namespace ourweb
