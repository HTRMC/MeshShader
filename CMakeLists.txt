cmake_minimum_required(VERSION 3.10)
project(UDPSystem)

set(CMAKE_CXX_STANDARD 17)

# Check for thread support
find_package(Threads REQUIRED)

# Server executable
add_executable(server udp_server.cpp)
target_link_libraries(server PRIVATE Threads::Threads)

# Client executable
add_executable(client udp_client.cpp)
target_link_libraries(client PRIVATE Threads::Threads)

# For Windows, we need to link against the Winsock library
if(WIN32)
    target_link_libraries(server PRIVATE wsock32 ws2_32)
    target_link_libraries(client PRIVATE wsock32 ws2_32)
endif()