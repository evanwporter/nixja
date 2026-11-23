cmake_minimum_required(VERSION 3.20)
project(example_sharedlib LANGUAGES C)

add_library(word1lib SHARED word1lib.c)
add_library(word2lib SHARED word2lib.c)

add_executable(main main.c)
target_link_libraries(main PRIVATE word1lib word2lib)
