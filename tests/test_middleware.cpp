#include <gtest/gtest.h>
#include <ourweb/app.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace ourweb;

namespace {

Request make_request(HttpMethod m, std::string path, std::string body = "") {
    return Request(m, std::move(path), {}, std::move(body), {});
}

// Records "<name>:before" / "<name>:after" into a shared trace.
MiddlewareFn tracer(std::shared_ptr<std::vector<std::string>> trace, std::string name) {
    return [trace, name](const Request&, MiddlewareNext next) {
        trace->push_back(name + ":before");
        Response r = next();
        trace->push_back(name + ":after");
        return r;
    };
}

} // namespace

TEST(Middleware, RunsInRegistrationOrderInAndReverseOut) {
    auto trace = std::make_shared<std::vector<std::string>>();
    App app;
    app.wrap(tracer(trace, "A")).wrap(tracer(trace, "B")).wrap(tracer(trace, "C"));
    app.get("/", [trace] {
        trace->push_back("handler");
        return text("ok");
    });

    auto resp = app.handle_request(make_request(HttpMethod::Get, "/"));
    EXPECT_EQ(resp.status_code(), HttpStatus::Ok);
    const std::vector<std::string> expected = {"A:before", "B:before", "C:before", "handler",
                                               "C:after",  "B:after",  "A:after"};
    EXPECT_EQ(*trace, expected);
}

TEST(Middleware, ShortCircuitSkipsInnerLayersAndHandler) {
    auto trace = std::make_shared<std::vector<std::string>>();
    App app;
    app.wrap(tracer(trace, "outer"));
    app.wrap([](const Request& req, MiddlewareNext next) {
        if (!req.header("authorization")) return text("denied", HttpStatus::Unauthorized);
        return next();
    });
    app.wrap(tracer(trace, "inner"));
    app.get("/", [trace] {
        trace->push_back("handler");
        return text("ok");
    });

    auto resp = app.handle_request(make_request(HttpMethod::Get, "/"));
    EXPECT_EQ(resp.status_code(), HttpStatus::Unauthorized);
    const std::vector<std::string> expected = {"outer:before", "outer:after"};
    EXPECT_EQ(*trace, expected);  // outer layers still see the rejection
}

TEST(Middleware, CanModifyResponse) {
    App app;
    app.wrap([](const Request&, MiddlewareNext next) {
        Response r = next();
        r.header("X-Served-By", "ourweb");
        return r;
    });
    app.get("/", [] { return text("ok"); });
    auto resp = app.handle_request(make_request(HttpMethod::Get, "/"));
    EXPECT_EQ(resp.headers().at("X-Served-By"), "ourweb");
}

TEST(Middleware, SeesTheSameRequestObjectEverywhereAndNoCopiesAreMade) {
    std::vector<const Request*> seen;
    App app;
    for (int i = 0; i < 3; ++i) {
        app.wrap([&seen](const Request& req, MiddlewareNext next) {
            seen.push_back(&req);
            Response r = next();
            seen.push_back(&req);  // still valid after next()
            return r;
        });
    }
    app.get("/x", [&seen](const Request& req) {
        seen.push_back(&req);
        return text("ok");
    });
    (void)app.handle_request(make_request(HttpMethod::Get, "/x", std::string(100000, 'b')));
    ASSERT_EQ(seen.size(), 7u);
    for (const Request* p : seen) EXPECT_EQ(p, seen.front());
}

TEST(Middleware, RequestIsIntactAfterNextEvenForByValueHandlers) {
    std::string logged_path, logged_body;
    App app;
    app.wrap([&](const Request& req, MiddlewareNext next) {
        Response r = next();
        logged_path = std::string(req.path());     // used to be moved-from by the handler
        logged_body = std::string(req.body());
        return r;
    });
    app.post("/consume/{id}", [](Request owned) { return text(std::string(owned.body())); });
    auto resp = app.handle_request(make_request(HttpMethod::Post, "/consume/7", std::string(500, 'z')));
    EXPECT_EQ(resp.body().size(), 500u);
    EXPECT_EQ(logged_path, "/consume/7");
    EXPECT_EQ(logged_body.size(), 500u);
}

TEST(Middleware, PathParametersAreVisibleToMiddleware) {
    std::string id;
    App app;
    app.wrap([&id](const Request& req, MiddlewareNext next) {
        if (auto v = req.path_param("id")) id = std::string(*v);
        return next();
    });
    app.get("/users/{id}", [] { return text("ok"); });
    (void)app.handle_request(make_request(HttpMethod::Get, "/users/42"));
    EXPECT_EQ(id, "42");
}

TEST(Middleware, WrapsNotFoundAndMethodNotAllowed) {
    std::vector<int> statuses;
    App app;
    app.wrap([&statuses](const Request&, MiddlewareNext next) {
        Response r = next();
        statuses.push_back(static_cast<int>(r.status_code()));
        return r;
    });
    app.get("/a", [] { return text("ok"); });
    auto nf = app.handle_request(make_request(HttpMethod::Get, "/missing"));
    auto na = app.handle_request(make_request(HttpMethod::Post, "/a"));
    EXPECT_EQ(nf.status_code(), HttpStatus::NotFound);
    EXPECT_EQ(na.status_code(), HttpStatus::MethodNotAllowed);
    EXPECT_EQ(na.headers().at("Allow"), "GET, HEAD");
    const std::vector<int> expected = {404, 405};
    EXPECT_EQ(statuses, expected);
}

TEST(Middleware, CallingNextTwiceFailsPredictablyAndHandlerRunsOnce) {
    int handler_calls = 0;
    int outer_seen = 0;
    App app;
    app.wrap([&outer_seen](const Request&, MiddlewareNext next) {
        Response r = next();
        outer_seen = static_cast<int>(r.status_code());
        return r;
    });
    app.wrap([](const Request&, MiddlewareNext next) {
        Response first = next();
        Response second = next();  // misuse: throws std::logic_error
        return second;
    });
    app.get("/", [&handler_calls] {
        ++handler_calls;
        return text("ok");
    });
    auto resp = app.handle_request(make_request(HttpMethod::Get, "/"));
    EXPECT_EQ(handler_calls, 1);
    EXPECT_EQ(resp.status_code(), HttpStatus::InternalServerError);
    EXPECT_EQ(outer_seen, 500);  // the outer layer still got a Response, not an exception
}

TEST(Middleware, ExceptionsBecome500AtTheNextBoundary) {
    int outer_seen = 0;
    App app;
    app.wrap([&outer_seen](const Request&, MiddlewareNext next) {
        Response r = next();  // must not throw
        outer_seen = static_cast<int>(r.status_code());
        return r;
    });
    app.wrap([](const Request&, MiddlewareNext) -> Response { throw std::runtime_error("secret detail"); });
    app.get("/", [] { return text("ok"); });
    auto resp = app.handle_request(make_request(HttpMethod::Get, "/"));
    EXPECT_EQ(outer_seen, 500);
    EXPECT_EQ(resp.status_code(), HttpStatus::InternalServerError);
    EXPECT_TRUE(std::string(resp.body()).find("secret") == std::string::npos);
}

TEST(Middleware, HandlerExceptionBecomes500WithoutLeakingDetails) {
    App app;
    app.get("/boom", []() -> Response { throw std::runtime_error("db password is hunter2"); });
    app.get("/boom2", []() -> Response { throw 42; });
    for (const char* path : {"/boom", "/boom2"}) {
        auto resp = app.handle_request(make_request(HttpMethod::Get, path));
        EXPECT_EQ(resp.status_code(), HttpStatus::InternalServerError);
        EXPECT_TRUE(std::string(resp.body()).find("hunter2") == std::string::npos);
    }
}

TEST(Middleware, NextIsConstructibleFromACallableForIsolatedTests) {
    LoggerMiddleware logger("[test]");
    Request req = make_request(HttpMethod::Get, "/iso");
    Response r = logger.invoke(req, [] { return text("inner"); });
    EXPECT_EQ(r.body(), "inner");
}

TEST(Middleware, NullMiddlewareRejected) {
    App app;
    EXPECT_THROW(app.wrap(std::shared_ptr<Middleware>{}), std::invalid_argument);
    EXPECT_THROW(app.wrap(MiddlewareFn{}), std::invalid_argument);
}
