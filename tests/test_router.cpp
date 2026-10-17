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

// ---------------------------------------------------------------------------
// Pattern validation, duplicates, specificity, decoding
// ---------------------------------------------------------------------------

namespace {
HandlerFn h(const char* tag) {
    return [tag](const Request&) { return text(tag); };
}
std::string run(Router& r, HttpMethod m, std::string_view path) {
    Request req;
    auto match = r.match(m, path, req);
    return match.handler ? std::string(match.handler(req).body()) : std::string("<none>");
}
} // namespace

TEST(RouterValidation, RejectsBadPatterns) {
    Router r;
    for (const char* bad : {"", "users", "/a//b", "/a/{}", "/a/{id", "/a/id}", "/a/{1x}", "/a/{a-b}",
                            "/a/x{id}", "/a/{id}.json", "/a/{id}{b}", "/a/{{id}}", "/a/{id}/{id}",
                            "/a?x=1", "/a#f", "/a/%20", "/a/*", "/a b", "//", "/a/{id}}"}) {
        EXPECT_THROW(r.add(HttpMethod::Get, bad, h("x")), RouteError);
    }
    EXPECT_TRUE(r.empty());  // failed registrations leave no residue
}

TEST(RouterValidation, AcceptsGoodPatterns) {
    for (const char* ok : {"/", "/a", "/a/", "/a/b/c", "/users/{id}", "/u/{user_id}/p/{_p2}", "/caf\xc3\xa9"}) {
        Router r;  // fresh router: "/a" and "/a/" are the same route
        EXPECT_NO_THROW(r.add(HttpMethod::Get, ok, h("x")));
    }
}

TEST(RouterValidation, ErrorMessageNamesTheProblem) {
    try {
        Router().add(HttpMethod::Get, "/a/{id}/{id}", h("x"));
        FAIL();
    } catch (const RouteError& e) {
        EXPECT_TRUE(std::string(e.what()).find("duplicate parameter name") != std::string::npos);
    }
}

TEST(RouterValidation, UnknownMethodAndEmptyHandlerRejected) {
    Router r;
    EXPECT_THROW(r.add(HttpMethod::Unknown, "/x", h("x")), RouteError);
    EXPECT_THROW(r.add(HttpMethod::Get, "/x", HandlerFn{}), RouteError);
}

TEST(RouterDuplicates, SameShapeDifferentParamNamesConflict) {
    Router r;
    r.add(HttpMethod::Get, "/users/{id}", h("a"));
    EXPECT_THROW(r.add(HttpMethod::Get, "/users/{uid}", h("b")), RouteError);
    EXPECT_NO_THROW(r.add(HttpMethod::Post, "/users/{uid}", h("c")));  // other method is fine
    EXPECT_EQ(r.size(), 2u);
}

TEST(RouterDuplicates, TrailingSlashIsTheSameRoute) {
    Router r;
    r.add(HttpMethod::Get, "/a", h("a"));
    EXPECT_THROW(r.add(HttpMethod::Get, "/a/", h("b")), RouteError);
    r.add(HttpMethod::Get, "/", h("root"));
    EXPECT_THROW(r.add(HttpMethod::Get, "/", h("root2")), RouteError);
}

TEST(RouterDuplicates, LiteralAndParamAtSamePositionAreNotDuplicates) {
    Router r;
    EXPECT_NO_THROW(r.add(HttpMethod::Get, "/users/me", h("me")));
    EXPECT_NO_THROW(r.add(HttpMethod::Get, "/users/{id}", h("id")));
}

TEST(RouterSpecificity, LiteralBeatsParamRegardlessOfRegistrationOrder) {
    for (bool literal_first : {true, false}) {
        Router r;
        if (literal_first) {
            r.add(HttpMethod::Get, "/users/me", h("me"));
            r.add(HttpMethod::Get, "/users/{id}", h("id"));
        } else {
            r.add(HttpMethod::Get, "/users/{id}", h("id"));
            r.add(HttpMethod::Get, "/users/me", h("me"));
        }
        EXPECT_EQ(run(r, HttpMethod::Get, "/users/me"), "me");
        EXPECT_EQ(run(r, HttpMethod::Get, "/users/42"), "id");
    }
}

TEST(RouterSpecificity, EarlierLiteralPositionWins) {
    Router r;
    r.add(HttpMethod::Get, "/{a}/x", h("param-first"));
    r.add(HttpMethod::Get, "/a/{b}", h("literal-first"));
    EXPECT_EQ(run(r, HttpMethod::Get, "/a/x"), "literal-first");
    EXPECT_EQ(run(r, HttpMethod::Get, "/b/x"), "param-first");
}

TEST(RouterMatching, ParamsDecodedPerSegmentAndOrdered) {
    Router r;
    r.add(HttpMethod::Get, "/u/{user}/p/{post}", [](const Request&) { return text("ok"); });
    Request req;
    auto m = r.match(HttpMethod::Get, "/u/a%20b%2Fc/p/x+y", req);
    ASSERT_TRUE(m.handler);
    ASSERT_EQ(req.path_params().size(), 2u);
    EXPECT_EQ(req.path_params()[0].first, "user");
    EXPECT_EQ(req.path_params()[0].second, "a b/c");   // %2F stays inside the segment
    EXPECT_EQ(req.path_params()[1].second, "x+y");       // '+' is literal in paths
    EXPECT_EQ(req.path_param_at(1)->first, "post");
    EXPECT_FALSE(req.path_param_at(2));
}

TEST(RouterMatching, EncodedSlashDoesNotSplitSegments) {
    Router r;
    r.add(HttpMethod::Get, "/a/{x}", h("one"));
    EXPECT_EQ(run(r, HttpMethod::Get, "/a/b%2Fc"), "one");
    EXPECT_EQ(run(r, HttpMethod::Get, "/a/b/c"), "<none>");
}

TEST(RouterMatching, EmptySegmentsAndMalformedEscapes) {
    Router r;
    r.add(HttpMethod::Get, "/users/{id}", h("u"));
    r.add(HttpMethod::Get, "/", h("root"));
    EXPECT_EQ(run(r, HttpMethod::Get, "/users//"), "<none>");   // empty param value
    EXPECT_EQ(run(r, HttpMethod::Get, "/users//x"), "<none>");
    EXPECT_EQ(run(r, HttpMethod::Get, "//users/1"), "<none>");  // empty leading segment
    EXPECT_EQ(run(r, HttpMethod::Get, "/users/%zz"), "<none>");
    EXPECT_EQ(run(r, HttpMethod::Get, "/users/1/"), "u");       // single trailing slash ok
    EXPECT_EQ(run(r, HttpMethod::Get, "/"), "root");
    EXPECT_EQ(run(r, HttpMethod::Get, ""), "root");
}

TEST(RouterMatching, AllowedMethodsDedupedAndOrdered) {
    Router r;
    r.add(HttpMethod::Delete, "/r/{id}", h("d"));
    r.add(HttpMethod::Post, "/r/{x}", h("p"));
    r.add(HttpMethod::Get, "/r/me", h("g"));
    Request req;
    auto m = r.match(HttpMethod::Put, "/r/me", req);
    EXPECT_FALSE(m.handler);
    ASSERT_TRUE(m.path_matched);
    ASSERT_EQ(m.allowed_methods.size(), 3u);
    EXPECT_EQ(m.allowed_methods[0], HttpMethod::Get);
    EXPECT_EQ(m.allowed_methods[1], HttpMethod::Post);
    EXPECT_EQ(m.allowed_methods[2], HttpMethod::Delete);
}

TEST(RouterMatching, HeadFallsBackToGetButPrefersExplicitHead) {
    Router r;
    r.add(HttpMethod::Get, "/g", h("get"));
    r.add(HttpMethod::Get, "/b", h("get-b"));
    r.add(HttpMethod::Head, "/b", h("head-b"));
    EXPECT_EQ(run(r, HttpMethod::Head, "/g"), "get");
    EXPECT_EQ(run(r, HttpMethod::Head, "/b"), "head-b");
}

TEST(RouterRegistration, RequiredPathParamsCheckedAgainstPattern) {
    Router r;
    EXPECT_THROW(r.add(HttpMethod::Get, "/a", h("x"), 1), RouteError);
    EXPECT_THROW(r.add(HttpMethod::Get, "/a/{x}", h("x"), 2), RouteError);
    EXPECT_NO_THROW(r.add(HttpMethod::Get, "/a/{x}/{y}", h("x"), 2));
}
