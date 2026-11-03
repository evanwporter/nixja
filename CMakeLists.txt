cmake_minimum_required(VERSION 3.20)

project(nixja LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

set(NINJA_BUILD_BINARY OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

find_package(PkgConfig REQUIRED)

pkg_check_modules(NIX_STORE REQUIRED IMPORTED_TARGET nix-store)

# The example binaries are copied into a Nix source tree and executed inside a
# sandbox. Keep their Nix shared libraries beside them, and search their own
# directory before the build-machine library paths.
find_library(NIX_STORE_SHARED_LIBRARY
    NAMES nixstore
    HINTS ${NIX_STORE_LIBRARY_DIRS}
    NO_DEFAULT_PATH
    REQUIRED)
find_library(NIX_UTIL_SHARED_LIBRARY
    NAMES nixutil
    HINTS ${NIX_STORE_LIBRARY_DIRS}
    NO_DEFAULT_PATH
    REQUIRED)

get_filename_component(NIX_STORE_SHARED_LIBRARY_REALPATH
    "${NIX_STORE_SHARED_LIBRARY}" REALPATH)
get_filename_component(NIX_UTIL_SHARED_LIBRARY_REALPATH
    "${NIX_UTIL_SHARED_LIBRARY}" REALPATH)
get_filename_component(NIX_STORE_SHARED_LIBRARY_NAME
    "${NIX_STORE_SHARED_LIBRARY_REALPATH}" NAME)
get_filename_component(NIX_UTIL_SHARED_LIBRARY_NAME
    "${NIX_UTIL_SHARED_LIBRARY_REALPATH}" NAME)

find_package(inja CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

get_target_property(FMT_SHARED_LIBRARY fmt::fmt IMPORTED_LOCATION_RELEASE)
if(NOT FMT_SHARED_LIBRARY)
    get_target_property(FMT_SHARED_LIBRARY fmt::fmt IMPORTED_LOCATION)
endif()
if(NOT FMT_SHARED_LIBRARY)
    message(FATAL_ERROR "Could not determine the shared library for fmt::fmt")
endif()
get_filename_component(FMT_SHARED_LIBRARY_REALPATH
    "${FMT_SHARED_LIBRARY}" REALPATH)
get_filename_component(FMT_SHARED_LIBRARY_NAME
    "${FMT_SHARED_LIBRARY_REALPATH}" NAME)
get_target_property(FMT_SHARED_LIBRARY_SONAME fmt::fmt IMPORTED_SONAME_RELEASE)
if(NOT FMT_SHARED_LIBRARY_SONAME)
    get_filename_component(FMT_SHARED_LIBRARY_SONAME
        "${FMT_SHARED_LIBRARY}" NAME)
endif()

add_subdirectory(external/ninja)

add_subdirectory(src/nixja)
add_subdirectory(src/nixja-gen)
add_subdirectory(src/nixja-scan-deps)

foreach(nixja_binary nixja nixja-gen nixja-scan-deps)
    set_property(TARGET ${nixja_binary} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
endforeach()

# TODO: Remove. This is hacky
add_custom_target(refresh-example-binaries ALL
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_SOURCE_DIR}/example/nixja"
    COMMAND ${CMAKE_COMMAND} -E create_hardlink
        "$<TARGET_FILE:nixja>" "${CMAKE_SOURCE_DIR}/example/nixja"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_SOURCE_DIR}/example/nixja-gen"
    COMMAND ${CMAKE_COMMAND} -E create_hardlink
        "$<TARGET_FILE:nixja-gen>" "${CMAKE_SOURCE_DIR}/example/nixja-gen"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_SOURCE_DIR}/example/nixja-scan-deps"
    COMMAND ${CMAKE_COMMAND} -E create_hardlink
        "$<TARGET_FILE:nixja-scan-deps>" "${CMAKE_SOURCE_DIR}/example/nixja-scan-deps"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NIX_STORE_SHARED_LIBRARY_REALPATH}"
        "${CMAKE_SOURCE_DIR}/example/${NIX_STORE_SHARED_LIBRARY_NAME}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NIX_UTIL_SHARED_LIBRARY_REALPATH}"
        "${CMAKE_SOURCE_DIR}/example/${NIX_UTIL_SHARED_LIBRARY_NAME}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FMT_SHARED_LIBRARY_REALPATH}"
        "${CMAKE_SOURCE_DIR}/example/${FMT_SHARED_LIBRARY_NAME}"
    COMMAND ${CMAKE_COMMAND} -E rm -f
        "${CMAKE_SOURCE_DIR}/example/${FMT_SHARED_LIBRARY_SONAME}"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "${FMT_SHARED_LIBRARY_NAME}"
        "${CMAKE_SOURCE_DIR}/example/${FMT_SHARED_LIBRARY_SONAME}"
    DEPENDS nixja nixja-gen nixja-scan-deps
    VERBATIM
)
