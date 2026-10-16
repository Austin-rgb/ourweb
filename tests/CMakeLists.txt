add_executable(ourweb_tests
    test_router.cpp
    test_parser.cpp
    test_response.cpp
    test_path.cpp
)

target_link_libraries(ourweb_tests
    PRIVATE
        ourweb::ourweb
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(ourweb_tests)
