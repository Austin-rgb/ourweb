#include <ourweb/app.hpp>

#include <iostream>

int main() {
    ourweb::App app;

    app.use_logger();

    app.get("/", [] {
        return ourweb::text("Hello, world!");
    });

    app.get("/health", [] {
        return ourweb::text("ok");
    });

    std::cout << "Starting OurWeb hello_world on 127.0.0.1:8080\n";
    app.run("127.0.0.1:8080");
    return 0;
}
