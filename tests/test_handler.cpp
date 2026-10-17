#include <gtest/gtest.h>
#include <ourweb/app.hpp>

#include <cstdint>
#include <string>

using namespace ourweb;

namespace {

struct CreateUser {
    std::string name;
    std::string email;
};
void to_json(nlohmann::json& j, const CreateUser& u) {
    j = nlohmann::json{{"name", u.name}, {"email", u.email}};
}
void from_json(const nlohmann::json& j, CreateUser& u) {
    j.at("name").get_to(u.name);
    j.at("email").get_to(u.email);
}

Request get(std::string path) { return Request(HttpMethod::Get, std::move(path), {}, "", {}); }

Request post_json(std::string path, std::string body,
                  std::string content_type = "application/json") {
    std::unordered_map<std::string, std::string> headers;
    if (!content_type.empty()) headers["content-type"] = std::move(content_type);
    return Request(HttpMethod::Post, std::move(path), std::move(headers), std::move(body), {});
}

std::string body_of(const Response& r) { return std::string(r.body()); }

// Free functions are valid handlers too (FR-05).
Response free_fn(Path<int> id) { return text("free:" + std::to_string(*id)); }
Response free_zero() { return text("zero"); }

} // namespace

TEST(HandlerSignatures, ZeroArgRequestByValueAndConstRef) {
    App app;
    app.get("/zero", [] { return text("zero"); });
    app.get("/byval", [](Request r) { return text(std::string(r.path())); });
    app.get("/cref", [](const Request& r) { return text(std::string(r.path())); });
    app.get("/fn0", free_zero);
    EXPECT_EQ(body_of(app.handle_request(get("/zero"))), "zero");
    EXPECT_EQ(body_of(app.handle_request(get("/byval"))), "/byval");
    EXPECT_EQ(body_of(app.handle_request(get("/cref"))), "/cref");
    EXPECT_EQ(body_of(app.handle_request(get("/fn0"))), "zero");
}

TEST(HandlerSignatures, ConvertibleReturnTypeAndStdFunction) {
    App app;
    std::function<Response(Request)> f = [](Request) { return text("std::function"); };
    app.get("/f", f);
    EXPECT_EQ(body_of(app.handle_request(get("/f"))), "std::function");
}

TEST(PathAdapter, IntParameter) {
    App app;
    app.get("/users/{id}", [](Path<int> id) { return text("user " + std::to_string(*id)); });
    app.get("/fn/{id}", free_fn);

    EXPECT_EQ(body_of(app.handle_request(get("/users/42"))), "user 42");
    EXPECT_EQ(body_of(app.handle_request(get("/users/-7"))), "user -7");
    EXPECT_EQ(body_of(app.handle_request(get("/fn/5"))), "free:5");
}

TEST(PathAdapter, InvalidConversionIs400AndHandlerNotCalled) {
    int calls = 0;
    App app;
    app.get("/users/{id}", [&calls](Path<int> id) {
        ++calls;
        return text(std::to_string(*id));
    });
    for (const char* bad : {"abc", "12abc", "+5", "99999999999999999999", "1.5", "%20", "0x10"}) {
        auto resp = app.handle_request(get(std::string("/users/") + bad));
        EXPECT_EQ(resp.status_code(), HttpStatus::BadRequest);
    }
    EXPECT_EQ(calls, 0);
    // Error body is the structured JSON format and does not echo client input.
    auto resp = app.handle_request(get("/users/abc"));
    EXPECT_TRUE(body_of(resp).find("invalid_parameter") != std::string::npos);
    EXPECT_TRUE(body_of(resp).find("abc") == std::string::npos);
    EXPECT_TRUE(body_of(resp).find("'id'") != std::string::npos);  // developer-chosen name is fine
}

TEST(PathAdapter, StringParameterIsDecoded) {
    App app;
    app.get("/hello/{name}", [](Path<std::string> n) { return text("hi " + *n); });
    EXPECT_EQ(body_of(app.handle_request(get("/hello/John%20Doe"))), "hi John Doe");
}

TEST(PathAdapter, OutOfRangeForNarrowTypes) {
    App app;
    app.get("/b/{v}", [](Path<std::uint8_t> v) { return text(std::to_string(*v)); });
    EXPECT_EQ(body_of(app.handle_request(get("/b/255"))), "255");
    EXPECT_EQ(app.handle_request(get("/b/256")).status_code(), HttpStatus::BadRequest);
    EXPECT_EQ(app.handle_request(get("/b/-1")).status_code(), HttpStatus::BadRequest);
}

TEST(PathAdapter, MultipleParametersBindByPosition) {
    App app;
    app.get("/u/{user}/p/{post}",
            [](Path<std::string> user, Path<int> post) { return text(*user + "#" + std::to_string(*post)); });
    EXPECT_EQ(body_of(app.handle_request(get("/u/ann/p/9"))), "ann#9");
    EXPECT_EQ(app.handle_request(get("/u/ann/p/x")).status_code(), HttpStatus::BadRequest);
}

TEST(PathAdapter, FewerPathArgsThanParamsIsFineAndUsesLeadingOnes) {
    App app;
    app.get("/a/{x}/b/{y}", [](Path<int> x) { return text(std::to_string(*x)); });
    EXPECT_EQ(body_of(app.handle_request(get("/a/3/b/4"))), "3");
}

TEST(PathAdapter, TooManyPathArgsFailsAtRegistration) {
    App app;
    EXPECT_THROW(app.get("/x", [](Path<int>) { return text("x"); }), RouteError);
    EXPECT_THROW(app.get("/x/{a}", [](Path<int>, Path<int>) { return text("x"); }), RouteError);
}

TEST(PathAdapter, CombinesWithRequestInAnyOrder) {
    App app;
    app.get("/r/{id}", [](const Request& req, Path<int> id) {
        return text(std::string(req.path()) + "=" + std::to_string(*id));
    });
    app.get("/s/{id}", [](Path<int> id, Request req) {
        return text(std::string(req.path()) + "=" + std::to_string(*id));
    });
    EXPECT_EQ(body_of(app.handle_request(get("/r/1"))), "/r/1=1");
    EXPECT_EQ(body_of(app.handle_request(get("/s/2"))), "/s/2=2");
}

TEST(PathAdapter, ConstRefAndRvalueRefParameters) {
    App app;
    app.get("/c/{id}", [](const Path<int>& id) { return text(std::to_string(*id)); });
    app.get("/m/{id}", [](Path<int>&& id) { return text(std::to_string(*id)); });
    EXPECT_EQ(body_of(app.handle_request(get("/c/8"))), "8");
    EXPECT_EQ(body_of(app.handle_request(get("/m/9"))), "9");
}

TEST(JsonAdapter, ValidBody) {
    App app;
    app.post("/users", [](Json<CreateUser> in) {
        return json(*in, HttpStatus::Created);
    });
    auto resp = app.handle_request(post_json("/users", R"({"name":"Ann","email":"a@x.io"})"));
    EXPECT_EQ(resp.status_code(), HttpStatus::Created);
    EXPECT_TRUE(body_of(resp).find("\"name\":\"Ann\"") != std::string::npos);
    EXPECT_EQ(resp.headers().at("Content-Type"), "application/json");
}

TEST(JsonAdapter, RejectionsDoNotCallHandlerAndDoNotLeakDetails) {
    int calls = 0;
    App app;
    app.post("/users", [&calls](Json<CreateUser>) {
        ++calls;
        return text("ok");
    });

    struct Case { const char* body; const char* ct; HttpStatus expected; };
    const Case cases[] = {
        {"{not json", "application/json", HttpStatus::BadRequest},
        {"", "application/json", HttpStatus::BadRequest},
        {R"({"name":"only"})", "application/json", HttpStatus::BadRequest},          // missing field
        {R"({"name":1,"email":2})", "application/json", HttpStatus::BadRequest},      // wrong types
        {"[]", "application/json", HttpStatus::BadRequest},
        {R"({"name":"a","email":"b"})", "text/plain", HttpStatus::UnsupportedMediaType},
        {R"({"name":"a","email":"b"})", "", HttpStatus::UnsupportedMediaType},         // no header
    };
    for (const auto& c : cases) {
        auto resp = app.handle_request(post_json("/users", c.body, c.ct));
        EXPECT_EQ(resp.status_code(), c.expected);
        const std::string b = body_of(resp);
        EXPECT_TRUE(b.find("key not found") == std::string::npos);  // no library exception text
        EXPECT_TRUE(b.find("\"error\"") != std::string::npos);
    }
    EXPECT_EQ(calls, 0);
}

TEST(JsonAdapter, ContentTypeVariantsAccepted) {
    App app;
    app.post("/u", [](Json<CreateUser> in) { return text(in->name); });
    for (const char* ct : {"application/json", "Application/JSON", "application/json; charset=utf-8",
                           "  application/json  ", "application/vnd.api+json"}) {
        auto resp = app.handle_request(post_json("/u", R"({"name":"n","email":"e"})", ct));
        EXPECT_EQ(resp.status_code(), HttpStatus::Ok);
    }
    for (const char* ct : {"application/jsonx", "text/json", "application/+json", "json"}) {
        auto resp = app.handle_request(post_json("/u", R"({"name":"n","email":"e"})", ct));
        EXPECT_EQ(resp.status_code(), HttpStatus::UnsupportedMediaType);
    }
}

TEST(CombinedAdapters, PathAndJsonTogether) {
    App app;
    app.put("/users/{id}", [](Path<int> id, Json<CreateUser> in) {
        return text(std::to_string(*id) + ":" + in->name);
    });
    auto req = Request(HttpMethod::Put, "/users/5", {{"content-type", "application/json"}},
                       R"({"name":"Zed","email":"z@z"})", {});
    EXPECT_EQ(body_of(app.handle_request(std::move(req))), "5:Zed");

    // First failing extractor wins; handler not called.
    auto bad_path = Request(HttpMethod::Put, "/users/x", {{"content-type", "application/json"}},
                            R"({"name":"Zed","email":"z@z"})", {});
    auto resp = app.handle_request(std::move(bad_path));
    EXPECT_EQ(resp.status_code(), HttpStatus::BadRequest);
    EXPECT_TRUE(body_of(resp).find("invalid_parameter") != std::string::npos);

    auto bad_body = Request(HttpMethod::Put, "/users/5", {{"content-type", "application/json"}}, "nope", {});
    EXPECT_EQ(app.handle_request(std::move(bad_body)).status_code(), HttpStatus::BadRequest);
}

TEST(CombinedAdapters, HandlerIsCopiedIntoRouteAndCapturedStateIsShared) {
    auto counter = std::make_shared<int>(0);
    App app;
    app.get("/n", [counter] {
        ++*counter;
        return text("ok");
    });
    (void)app.handle_request(get("/n"));
    (void)app.handle_request(get("/n"));
    EXPECT_EQ(*counter, 2);
}

TEST(AppConfiguration, RouteErrorsSurfaceFromApp) {
    App app;
    app.get("/users/{id}", [] { return text("a"); });
    EXPECT_THROW(app.get("/users/{other}", [] { return text("b"); }), RouteError);
    EXPECT_THROW(app.get("no-slash", [] { return text("b"); }), RouteError);
    EXPECT_THROW(app.get("/x/{id", [] { return text("b"); }), RouteError);
}
