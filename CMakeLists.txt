cmake_minimum_required(VERSION 3.20)

project(nixja LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NINJA_BUILD_BINARY OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

find_package(inja CONFIG REQUIRED)

add_subdirectory(external/ninja)

add_subdirectory(src/nixja)

add_subdirectory(src/nixja-gen)
