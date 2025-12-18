cmake_minimum_required(VERSION 3.16)
project(udp_sender LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(udp_sender
  src/UdpSender.cpp
)
target_include_directories(udp_sender PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(udp_sender_tests
  tests/test_udp_sender.cpp
)
target_link_libraries(udp_sender_tests PRIVATE udp_sender)

enable_testing()
add_test(NAME udp_sender_tests COMMAND udp_sender_tests)