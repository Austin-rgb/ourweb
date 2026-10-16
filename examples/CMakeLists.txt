add_executable(hello_world hello_world.cpp)
target_link_libraries(hello_world PRIVATE ourweb::ourweb)

add_executable(users_api users_api.cpp)
target_link_libraries(users_api PRIVATE ourweb::ourweb)
