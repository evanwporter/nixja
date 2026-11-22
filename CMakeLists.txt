cmake_minimum_required(VERSION 3.20)

project(nixja LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

set(NINJA_BUILD_BINARY OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NIXJA_VERSION)
string(STRIP "${NIXJA_VERSION}" NIXJA_VERSION)

find_package(LLVM CONFIG REQUIRED)

include("${CMAKE_SOURCE_DIR}/cmake/toolchain.cmake")

find_package(argparse CONFIG REQUIRED)

find_package(Clang CONFIG REQUIRED)

add_subdirectory(external)

add_subdirectory(src/common)
add_subdirectory(src/nixja)
add_subdirectory(src/nixja-scan-deps)
add_subdirectory(src/nixja-task)
