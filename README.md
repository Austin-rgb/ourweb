# OurWeb

**Version 0.1.0 — Initial Implementation**

A lightweight, Actix-inspired HTTP web framework written in modern C++20.

OurWeb aims to provide an ergonomic API for building web servers and HTTP applications without forcing developers to deal with low-level networking, request parsing, or response construction.

## Features (v0.1.0)

- Simple route registration (`GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`)
- Literal paths and named path parameters (`/users/{id}`)
- Typed handler parameters: `Path<T>` (path parameters) and `Json<T>` (request body) are parsed for you, and a bad value is a 400/415 before your handler runs
- JSON request/response helpers (via nlohmann/json)
- Application state sharing (`StateRegistry`; capture a `shared_ptr` in your handlers)
- Predictable, copy-free middleware chaining (including a request logger)
- Structured HTTP error responses
- Strict HTTP/1.1 parsing with configurable request limits and timeouts
- Graceful shutdown that joins every worker thread
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

### Sanitizers

```bash
cmake -B build-asan -DOURWEB_ENABLE_ASAN=ON -DOURWEB_ENABLE_UBSAN=ON && cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
cmake -B build-tsan -DOURWEB_ENABLE_TSAN=ON && cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure
```

## Handlers

A handler is any plain function or non-generic lambda with a `const` call
operator that returns `Response`. Parameters are `Request`, `const Request&`
(zero-copy, preferred), or extractors, in any order:

```cpp
app.get("/users/{id}", [](Path<int> id) { return text(std::to_string(*id)); });

app.put("/users/{id}", [](Path<int> id, Json<CreateUser> body) {
    return json(*body);
});

app.get("/echo", [](const Request& req) { return text(std::string(req.path())); });
```

* The k-th `Path<>` parameter binds to the k-th `{param}` of the route pattern.
  Path values are percent-decoded. `Path<T>` supports `std::string` and every
  integral type except `bool` (add more via `detail::path_converter<T>`).
* `Json<T>` requires `Content-Type: application/json` (or `application/*+json`)
  and uses nlohmann's `from_json`. Failures never leak parser details:
  415 wrong content type, 400 empty/invalid JSON or wrong shape.
* Extractors run left to right and stop at the first failure; the handler is not
  called. Exceptions thrown by a handler become a 500 with a generic body.
* Unsupported signatures (generic lambdas, overload sets, `mutable` lambdas,
  non-copyable callables) are rejected at compile time with a descriptive message.
* Add your own extractor by specialising `FromRequest<T>` (see `extractor.hpp`).

## Routing

* Patterns start with `/`; a segment is literal text or exactly `{name}`
  (`[A-Za-z_][A-Za-z0-9_]*`). Invalid patterns (`/a//b`, `/a/x{id}`, `/a/{id}/{id}`,
  `?`, `#`, `*`, `%`, ...) throw `RouteError` at registration.
* Two routes with the same method and *shape* are duplicates and throw
  `RouteError`: `/users/{id}` conflicts with `/users/{uid}`, and `/a` with `/a/`.
* A literal segment beats a parameter regardless of registration order
  (`/users/me` is chosen over `/users/{id}`).
* `HEAD` falls back to the matching `GET` route (body omitted). Known path with
  the wrong method: 405 with an `Allow` header. Unknown path: 404.

## Middleware

```cpp
app.wrap(A); app.wrap(B);   // A-before, B-before, handler, B-after, A-after
```

Middleware receives the one and only `const Request&` (no copies; still valid
after `next()` returns), calls `next()` at most once, or returns its own
response to short-circuit. `next()` never throws: an exception in an inner layer
becomes a 500 response. Details in `ourweb/middleware.hpp`.

## Server behaviour and limits

Tune via `app.options(ServerOptions{...})` (see `server_options.hpp`):
body size (default 1 MiB, 413), header block (16 KiB, 431), request target
(8 KiB, 414), header count (100), max connections (1024, then 503), read /
request / write timeouts (408 for a client that stalls mid-request), and the
shutdown grace period.

The parser rejects bare LF/CR, NUL bytes, obsolete line folding, whitespace
before `:`, invalid or conflicting `Content-Length`, `Content-Length` together
with `Transfer-Encoding`, duplicate `Host`, bad versions/methods, and malformed
percent-encodings in the path or query string. Responses are always correctly
framed (`Content-Length` is computed by the server) and responses are written
with a loop that handles partial writes; a client that disconnects mid-response
cannot terminate the process (no SIGPIPE).

### Shutdown

`App::run()` blocks until `App::stop()` (thread-safe, idempotent, callable from a
handler; **not** async-signal-safe). Stopping closes the listening socket,
drops idle and half-read connections, lets requests that are already in a
handler finish (up to `shutdown_grace` for their sockets), and joins every worker
thread before `run()` returns. Handlers cannot be cancelled, so a handler that
never returns blocks shutdown. Configuration (routes, middleware, state, options)
is frozen while running. Don't move a running `App`.

## Known limitations (0.1.0)

* One request per connection (`Connection: close`); no keep-alive.
* Request bodies must use `Content-Length`; chunked uploads get `501`.
* Thread-per-connection (bounded by `max_connections`); handlers are synchronous.
* `State<T>` is not yet available as a handler parameter (capture a `shared_ptr`).
* No TLS. POSIX sockets only.

## Requirements

- C++20 compiler (GCC 10+, Clang 12+, MSVC 2019+)
- CMake 3.16+
- Internet access on first configure (FetchContent pulls nlohmann/json and GoogleTest)
- A POSIX system (Linux/macOS): the current networking layer uses POSIX sockets

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
