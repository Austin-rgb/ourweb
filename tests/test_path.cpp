#include <gtest/gtest.h>
#include <ourweb/path.hpp>
#include <ourweb/request.hpp>

using namespace ourweb;

TEST(Path, ExtractInt) {
    Request req;
    req.set_path_params({{"id", "42"}});
    auto p = extract_path<int>(req, "id");
    ASSERT_TRUE(p);
    EXPECT_EQ(**p, 42);
}

TEST(Path, ExtractString) {
    Request req;
    req.set_path_params({{"name", "alice"}});
    auto p = extract_path<std::string>(req, "name");
    ASSERT_TRUE(p);
    EXPECT_EQ(**p, "alice");
}

TEST(Path, InvalidInt) {
    Request req;
    req.set_path_params({{"id", "abc"}});
    auto p = extract_path<int>(req, "id");
    EXPECT_FALSE(p);
}

TEST(Path, Missing) {
    Request req;
    auto p = extract_path<int>(req, "id");
    EXPECT_FALSE(p);
}
