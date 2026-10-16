#pragma once

#include "ourweb/app.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ourweb {

class Server {
public:
    explicit Server(App& app);
    ~Server();

    void run(std::string_view address);
    void stop();

private:
    App& app_;
    std::atomic<bool> running_{false};
};

} // namespace ourweb
