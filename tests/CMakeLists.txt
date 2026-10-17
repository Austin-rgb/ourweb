add_executable(ourweb_tests
    test_router.cpp
    test_parser.cpp
    test_response.cpp
    test_path.cpp
    test_middleware.cpp
    test_handler.cpp
    test_server.cpp
)

target_link_libraries(ourweb_tests
    PRIVATE
        ourweb::ourweb
        GTest::gtest_main
)

# Tests reach into internal headers (parser, response writer).
target_include_directories(ourweb_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

include(GoogleTest)
# The server tests use real sockets and timers; give every test a hard timeout
# so a regression shows up as a failure rather than a hung CI job.
gtest_discover_tests(ourweb_tests PROPERTIES TIMEOUT 120)
