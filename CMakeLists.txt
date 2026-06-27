cmake_minimum_required(VERSION 3.16)
project(matching_engine_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(ENABLE_SANITIZERS "Build with address+undefined sanitizers" OFF)

find_package(Threads REQUIRED)

add_library(matching_engine
    src/json.cpp
    src/matching_engine.cpp
    src/net.cpp
    src/protocol.cpp
    src/recovery.cpp
    src/sharded.cpp
)
target_include_directories(matching_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(matching_engine PUBLIC Threads::Threads)

add_library(matching_engine_warnings INTERFACE)
target_compile_options(matching_engine_warnings INTERFACE -Wall -Wextra)
if(ENABLE_SANITIZERS)
    target_compile_options(matching_engine_warnings INTERFACE -fsanitize=address,undefined -g)
    target_link_options(matching_engine_warnings INTERFACE -fsanitize=address,undefined)
endif()
target_link_libraries(matching_engine PRIVATE matching_engine_warnings)

add_executable(matching_engine_demo src/main.cpp)
target_link_libraries(matching_engine_demo PRIVATE matching_engine matching_engine_warnings)

enable_testing()
add_executable(matching_engine_tests tests/test_engine.cpp)
target_link_libraries(matching_engine_tests PRIVATE matching_engine matching_engine_warnings)
add_test(NAME matching_engine_tests COMMAND matching_engine_tests)
