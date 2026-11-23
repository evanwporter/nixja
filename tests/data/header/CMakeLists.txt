cmake_minimum_required(VERSION 3.20)
project(example_header LANGUAGES CXX)

add_executable(hello main.cpp)
target_compile_features(hello PRIVATE cxx_std_14)
target_include_directories(hello PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
