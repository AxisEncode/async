cmake_minimum_required(VERSION 3.16)
project(async_io_cpp20 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(async_example examples/main.cpp)
target_include_directories(async_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
