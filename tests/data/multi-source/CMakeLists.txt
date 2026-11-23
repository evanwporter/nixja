cmake_minimum_required(VERSION 3.20)
project(example_incremental LANGUAGES C)

add_executable(main main.c util.c)
target_include_directories(main PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
