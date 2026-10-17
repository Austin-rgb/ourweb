cmake_minimum_required(VERSION 3.16)
project(OurWeb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(OURWEB_BUILD_EXAMPLES "Build example applications" ON)
option(OURWEB_BUILD_TESTS "Build tests" ON)
option(OURWEB_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(OURWEB_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(OURWEB_ENABLE_TSAN "Enable ThreadSanitizer (cannot be combined with ASan)" OFF)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

if(OURWEB_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(OURWEB_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=undefined)
endif()
if(OURWEB_ENABLE_TSAN)
    if(OURWEB_ENABLE_ASAN)
        message(FATAL_ERROR "OURWEB_ENABLE_TSAN cannot be combined with OURWEB_ENABLE_ASAN")
    endif()
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# Dependencies via FetchContent
include(FetchContent)

# Prefer extraction-time timestamps for URL downloads (CMP0135)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# Networking: POSIX sockets used for the initial Linux scaffold.
# Future versions will integrate standalone Asio or Boost.Asio.
find_package(Threads REQUIRED)

# nlohmann/json
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)

# GoogleTest (only if tests enabled)
if(OURWEB_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

# OurWeb library
add_library(ourweb
    src/app.cpp
    src/request.cpp
    src/response.cpp
    src/router.cpp
    src/server.cpp
    src/http_parser.cpp
    src/url.cpp
    src/socket_io.cpp
    src/response_writer.cpp
    src/middleware.cpp
    src/error.cpp
    src/http.cpp
)

add_library(ourweb::ourweb ALIAS ourweb)

target_include_directories(ourweb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ourweb
    PUBLIC
        nlohmann_json::nlohmann_json
        Threads::Threads
)

# Note: the current networking layer uses POSIX sockets (Linux/macOS).
# Windows support arrives with the planned Asio backend.

# Examples
if(OURWEB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Tests
if(OURWEB_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Installation (simple install of library + headers; full package export
# deferred until nlohmann_json install story is finalized)
include(GNUInstallDirs)
install(TARGETS ourweb
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ourweb DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
