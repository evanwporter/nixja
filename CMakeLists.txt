cmake_minimum_required(VERSION 3.20)

project(nixja LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NINJA_BUILD_BINARY OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NIXJA_VERSION)
string(STRIP "${NIXJA_VERSION}" NIXJA_VERSION)

find_package(LLVM CONFIG REQUIRED)
find_package(Clang CONFIG REQUIRED)

find_package(argparse CONFIG REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(TREE_SITTER REQUIRED IMPORTED_TARGET tree-sitter)
pkg_check_modules(TREE_SITTER_BASH REQUIRED IMPORTED_TARGET tree-sitter-bash)

include("${CMAKE_SOURCE_DIR}/cmake/toolchain.cmake")

add_subdirectory(external/ninja)
# Ninja enables IPO for release builds, but these object files are archived
# into libnixja with the toolchain's regular archiver.
set_target_properties(libninja libninja-re2c PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
)
add_subdirectory(external/expected-fs)
add_subdirectory(external/libpatchelf)

add_subdirectory(src/common)
add_subdirectory(src/nixja)
add_subdirectory(src/nixja-scan-deps)
add_subdirectory(src/nixja-task)
add_subdirectory(src/clang-scan-deps)

option(NIXJA_BUILD_TESTS "Build nixja tests" ON)
if(NIXJA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

set(EXAMPLE_BINARIES)
foreach(target IN ITEMS nixja nixja-scan-deps nixja-task)
    set(example_binary "${CMAKE_SOURCE_DIR}/tests/data/${target}")
    add_custom_command(
        OUTPUT "${example_binary}"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${example_binary}"
        COMMAND ${CMAKE_COMMAND} -E create_hardlink
            "$<TARGET_FILE:${target}>" "${example_binary}"
        DEPENDS ${target}
        VERBATIM
    )
    list(APPEND EXAMPLE_BINARIES "${example_binary}")
endforeach()

add_custom_target(refresh-example-binaries ALL
    DEPENDS ${EXAMPLE_BINARIES}
)
