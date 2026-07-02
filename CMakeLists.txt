cmake_minimum_required(VERSION 3.16)
project(order_matching_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

add_executable(benchmark benchmark/benchmark_main.cpp)
target_link_libraries(benchmark pthread)

add_executable(demo src/demo.cpp)
target_link_libraries(demo pthread)

enable_testing()
add_executable(test_matching tests/test_matching.cpp)
target_link_libraries(test_matching pthread)
add_test(NAME matching_tests COMMAND test_matching)
