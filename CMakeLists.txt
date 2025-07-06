cmake_minimum_required(VERSION 3.16)
project(matching_engine_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(matching_engine
    src/matching_engine.cpp
)
target_include_directories(matching_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(matching_engine PRIVATE -Wall -Wextra)

add_executable(matching_engine_demo src/main.cpp)
target_link_libraries(matching_engine_demo PRIVATE matching_engine)

enable_testing()
add_executable(matching_engine_tests tests/test_engine.cpp)
target_link_libraries(matching_engine_tests PRIVATE matching_engine)
add_test(NAME matching_engine_tests COMMAND matching_engine_tests)
