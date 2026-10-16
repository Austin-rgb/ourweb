# OurWeb

**Version 0.1.0 — Initial Implementation**

A lightweight, Actix-inspired HTTP web framework written in modern C++20.

OurWeb aims to provide an ergonomic API for building web servers and HTTP applications without forcing developers to deal with low-level networking, request parsing, or response construction.

## Features (v0.1.0)

- Simple route registration (`GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`)
- Literal paths and named path parameters (`/users/{id}`)
- Typed path parameter extraction (`Path<int>`, `Path<std::string>`)
- JSON request/response helpers (via nlohmann/json)
- Application state sharing (`State<T>`)
- Basic middleware support (including a request logger)
- Structured HTTP error responses
- Configurable request body size limit
- Synchronous handlers (async/coroutine support planned for a future release)
- CMake-based build with FetchContent dependencies
- Initial networking uses POSIX sockets (Linux); Asio integration planned

## Quick Start

```cpp
#include <ourweb/app.hpp>

int main() {
    ourweb::App app;

    app.get("/", [] {
        return ourweb::text("Hello, world!");
    });

    app.run("127.0.0.1:8080");
}
```

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Run the hello-world example

```bash
./build/examples/hello_world
```

Then:

```bash
curl http://127.0.0.1:8080/
```

### Run tests

```bash
cmake -B build -DOURWEB_BUILD_TESTS=ON
cmake --build build -j
cd build && ctest --output-on-failure
```

## Requirements

- C++20 compiler (GCC 10+, Clang 12+, MSVC 2019+)
- CMake 3.16+
- Internet access on first configure (FetchContent pulls Asio, nlohmann/json, GoogleTest)

## Project Layout

```
include/ourweb/     Public headers
src/                Implementation
examples/           Runnable examples
tests/              Unit tests
```

## License

License to be selected before public distribution.

## Roadmap (post-0.1.0)

- Coroutine / async handlers (`Task<Response>`)
- More sophisticated extractors and typed query parameters
- Streaming responses
- TLS support
- Improved routing (wildcards, groups)
- Production hardening and benchmarks
