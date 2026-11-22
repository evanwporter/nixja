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

add_subdirectory(external/ninja)
add_subdirectory(external/expected-fs)

add_subdirectory(src/common)
add_subdirectory(src/nixja)
add_subdirectory(src/nixja-scan-deps)
add_subdirectory(src/nixja-task)

function(add_nixja_hardlinks dir)
    foreach(target IN ITEMS nixja nixja-scan-deps nixja-task)
        list(APPEND HARDLINK_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E rm -f "${dir}/${target}"
            COMMAND ${CMAKE_COMMAND} -E create_hardlink
                "$<TARGET_FILE:${target}>" "${dir}/${target}"
        )
    endforeach()

    set(HARDLINK_COMMANDS "${HARDLINK_COMMANDS}" PARENT_SCOPE)
endfunction()

set(HARDLINK_COMMANDS)

add_nixja_hardlinks("${CMAKE_SOURCE_DIR}/tests/data")

add_custom_target(refresh-example-binaries ALL
    ${HARDLINK_COMMANDS}
    DEPENDS nixja nixja-scan-deps
    VERBATIM
)
