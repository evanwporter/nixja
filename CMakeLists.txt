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

include("${CMAKE_SOURCE_DIR}/cmake/toolchain.cmake")

add_subdirectory(external/ninja)

add_subdirectory(src/common)
add_subdirectory(src/nixja)
add_subdirectory(src/nixja-gen)
add_subdirectory(src/nixja-scan-deps)

# Keep the executables beside the example inputs. Their Nix store RUNPATH
# resolves shared libraries without a copied runtime closure.
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
    DEPENDS nixja nixja-gen nixja-scan-deps
    VERBATIM
)
