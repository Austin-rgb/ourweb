#include <gtest/gtest.h>
#include <ourweb/response.hpp>
#include <ourweb/json.hpp>

using namespace ourweb;

TEST(Response, TextConvenience) {
    auto r = text("hello");
    EXPECT_EQ(r.status_code(), HttpStatus::Ok);
    EXPECT_EQ(r.body(), "hello");
    auto ct = r.headers().find("Content-Type");
    ASSERT_NE(ct, r.headers().end());
    EXPECT_TRUE(ct->second.find("text/plain") != std::string::npos);
}

TEST(Response, JsonConvenience) {
    struct Point { int x; int y; };
    // Use nlohmann directly via json()
    nlohmann::json j = {{"x", 1}, {"y", 2}};
    auto r = json(j);
    EXPECT_EQ(r.status_code(), HttpStatus::Ok);
    EXPECT_TRUE(r.body().find("\"x\":1") != std::string::npos ||
                r.body().find("\"x\": 1") != std::string::npos);
}

TEST(Response, StatusAndHeader) {
    auto r = Response{}
        .status(HttpStatus::Created)
        .header("Location", "/users/1")
        .body("Created");
    EXPECT_EQ(r.status_code(), HttpStatus::Created);
    EXPECT_EQ(r.body(), "Created");
    auto loc = r.headers().find("Location");
    ASSERT_NE(loc, r.headers().end());
    EXPECT_EQ(loc->second, "/users/1");
}

TEST(Response, InvalidHeaderThrows) {
    Response r;
    EXPECT_THROW(r.header("Bad\nName", "value"), std::invalid_argument);
    EXPECT_THROW(r.header("Ok", "bad\r\nvalue"), std::invalid_argument);
}
