
# TUTORIAL: Building a simple web service with OurWeb

This tutorial walks you through building a small REST-style web service using the OurWeb framework in this repository. It covers a minimal "users" service with route handlers, path parameters, JSON bodies, middleware, in-memory state, building, testing, and a Dockerfile for deployment.

Target audience: C++ developers familiar with modern C++ (C++20) and CMake.

Prerequisites

- C++20 compiler (GCC 10+, Clang 12+, MSVC 2019+)
- CMake 3.16+
- Git and Internet access on first configure (FetchContent pulls dependencies)
- A POSIX-compatible system (Linux/macOS) for the current networking layer

Quick build steps (run from the repo root)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Step 0 — Create a new example directory

Create a new example in examples/simple_users. We'll add a single-file example main and a small CMake target.

- examples/simple_users/main.cpp

```cpp
#include <ourweb/app.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// A tiny user model
struct User {
    int id;
    std::string name;
};

// JSON (de)serializers used by ourweb::Json<T> (nlohmann::from_json/to_json)
inline void to_json(json& j, const User& u) {
    j = json{{"id", u.id}, {"name", u.name}};
}
inline void from_json(const json& j, User& u) {
    j.at("id").get_to(u.id);
    j.at("name").get_to(u.name);
}

int main() {
    ourweb::App app;

    // In-memory state shared across handlers (thread-safe with a mutex)
    struct Store {
        std::unordered_map<int, User> users;
        int next_id = 1;
        std::mutex m;
    };
    auto store = std::make_shared<Store>();

    // Simple request logger middleware
    app.wrap([/*capture nothing*/](const ourweb::Request& req, auto next) {
        // This middleware runs before the handler
        fprintf(stderr, "%s %s\n", req.method().c_str(), req.path().c_str());
        auto res = next(); // call next layer / handler
        // This runs after the handler returns
        return res;
    });

    // GET /users -> list users
    app.get("/users", [store]() -> ourweb::Response {
        json j = json::array();
        {
            std::scoped_lock lock(store->m);
            for (auto const& [k, u] : store->users) j.push_back(u);
        }
        return ourweb::json(j);
    });

    // POST /users -> create a user from JSON body
    app.post("/users", [store](ourweb::Json<User> body) -> ourweb::Response {
        User u = *body; // validated and parsed by the extractor
        std::scoped_lock lock(store->m);
        u.id = store->next_id++;
        store->users[u.id] = u;
        return ourweb::json(u).with_status(201); // 201 Created
    });

    // GET /users/{id} -> fetch a single user
    app.get("/users/{id}", [store](ourweb::Path<int> id) {
        int uid = *id;
        std::scoped_lock lock(store->m);
        auto it = store->users.find(uid);
        if (it == store->users.end()) {
            return ourweb::text("Not Found").with_status(404);
        }
        return ourweb::json(it->second);
    });

    // PUT /users/{id} -> replace user
    app.put("/users/{id}", [store](ourweb::Path<int> id, ourweb::Json<User> body) {
        int uid = *id;
        User u = *body;
        std::scoped_lock lock(store->m);
        if (store->users.find(uid) == store->users.end()) {
            return ourweb::text("Not Found").with_status(404);
        }
        u.id = uid; // ensure ID stays in sync with path
        store->users[uid] = u;
        return ourweb::json(u);
    });

    // Simple health check
    app.get("/health", [] { return ourweb::text("OK"); });

    // Optional: tune server options (see server_options.hpp)
    ourweb::ServerOptions opts;
    opts.max_connections = 1024;
    opts.body_max_bytes = 1024 * 1024; // 1 MiB
    app.options(opts);

    // Run the server
    app.run("127.0.0.1:8080");
}
```

Notes about the example

- Handlers use ourweb::Json<T>, ourweb::Path<T>, and the convenience helpers ourweb::json(...) and ourweb::text(...), as shown in the README.
- The library's Json<T> extractor runs validation and parsing before the handler runs and returns appropriate 400/415 responses on bad input.
- We store state in a shared_ptr and capture it in handlers — this is the recommended approach until State<T> handler parameters are available.
- The example uses a mutex to keep the in-memory map thread-safe. The framework runs handlers synchronously per connection, but there can be multiple worker threads.

Step 1 — Add a CMake target for the example

Edit CMakeLists.txt at the repo root or add a new one under examples/simple_users/CMakeLists.txt. A minimal example target:

```cmake
add_executable(simple_users examples/simple_users/main.cpp)
target_link_libraries(simple_users PRIVATE ourweb::ourweb)
add_custom_target(run_simple_users COMMAND $<TARGET_FILE:simple_users>)
```

Adjust the target names to match the project's existing conventions. If the project already exposes a CMake target for the library (recommended name: ourweb::ourweb), link against it.

Build and run the example

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/examples/simple_users/simple_users
```

You can test the service with curl

```bash
# health
curl http://127.0.0.1:8080/health

# create a user
curl -v -X POST -H "Content-Type: application/json" \
  -d '{"name":"alice"}' http://127.0.0.1:8080/users

# list users
curl http://127.0.0.1:8080/users

# get user 1
curl http://127.0.0.1:8080/users/1
```

Middleware examples

- Logging (shown above) is a common middleware. Wrap order is important: app.wrap(A); app.wrap(B); results in A-before, B-before, handler, B-after, A-after.
- Middleware may short-circuit and return a response without calling next(). Use this for authentication, rate-limiting, etc.

Tests

- The repository contains a tests/ infrastructure. You can add unit/integration tests that start the App on a loopback address and exercise it using an HTTP client or raw sockets.
- To run the project's tests:

```bash
cmake -B build -DOURWEB_BUILD_TESTS=ON
cmake --build build -j
cd build && ctest --output-on-failure
```

Dockerfile (simple)

This example creates a minimal Dockerfile to build and run the example in a container.

```dockerfile
FROM ubuntu:22.04 AS build
RUN apt-get update && apt-get install -y build-essential cmake git pkg-config
WORKDIR /src
COPY . /src
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y ca-certificates
WORKDIR /app
COPY --from=build /src/build/examples/simple_users/simple_users /app/simple_users
EXPOSE 8080
CMD ["/app/simple_users"]
```

Notes and troubleshooting

- If you see JSON parsing errors when POSTing, confirm the Content-Type header is set to `application/json` (or `application/*+json`). The Json<T> extractor rejects requests with wrong/missing content type with 415.
- The framework currently supports only Content-Length request bodies (no chunked encoding). Large uploads may be rejected by body_max_bytes.
- Handlers are synchronous. A long-running handler will block shutdown until it returns (up to the configured shutdown grace period).
- If you need async/streaming or TLS, watch the roadmap — those are planned for future releases.

Next steps / ideas to extend the tutorial

- Add query-parameter extractors and show typed query parsing.
- Add authentication middleware (JWT header verification) and examples of short-circuiting responses.
- Show how to integrate a real database (SQLite / PostgreSQL) and connection pooling in the shared state.
- Demonstrate graceful shutdown and integration tests that assert clean shutdown behavior.

That's it — you now have a minimal, working web service built with OurWeb. Modify the example to match your application's routing and state needs.