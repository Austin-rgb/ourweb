#include <gtest/gtest.h>
#include <ourweb/router.hpp>
#include <ourweb/request.hpp>
#include <ourweb/response.hpp>

using namespace ourweb;

TEST(Router, MatchesLiteral) {
    Router r;
    bool called = false;
    r.add(HttpMethod::Get, "/hello", [&](Request) {
        called = true;
        return text("ok");
    });

    Request req;
    auto m = r.match(HttpMethod::Get, "/hello", req);
    ASSERT_TRUE(m.handler);
    auto resp = m.handler(std::move(req));
    EXPECT_TRUE(called);
    EXPECT_EQ(resp.status_code(), HttpStatus::Ok);
}

TEST(Router, PathParameter) {
    Router r;
    r.add(HttpMethod::Get, "/users/{id}", [](Request req) {
        auto id = req.path_param("id");
        EXPECT_TRUE(id);
        EXPECT_EQ(*id, "42");
        return text(*id);
    });

    Request req;
    auto m = r.match(HttpMethod::Get, "/users/42", req);
    ASSERT_TRUE(m.handler);
    auto resp = m.handler(std::move(req));
    EXPECT_EQ(resp.body(), "42");
}

TEST(Router, MethodNotAllowed) {
    Router r;
    r.add(HttpMethod::Get, "/res", [](Request) { return text("get"); });

    Request req;
    auto m = r.match(HttpMethod::Post, "/res", req);
    EXPECT_FALSE(m.handler);
    EXPECT_TRUE(m.path_matched);
    EXPECT_EQ(m.allowed_methods.size(), 1u);
    EXPECT_EQ(m.allowed_methods[0], HttpMethod::Get);
}

TEST(Router, NotFound) {
    Router r;
    r.add(HttpMethod::Get, "/only", [](Request) { return text("x"); });

    Request req;
    auto m = r.match(HttpMethod::Get, "/other", req);
    EXPECT_FALSE(m.handler);
    EXPECT_FALSE(m.path_matched);
}

TEST(Router, DuplicateThrows) {
    Router r;
    r.add(HttpMethod::Get, "/x", [](Request) { return text("a"); });
    EXPECT_THROW(
        r.add(HttpMethod::Get, "/x", [](Request) { return text("b"); }),
        std::runtime_error);
}
