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

add_subdirectory(src/nixja-scan-deps)

add_custom_target(refresh-root-binaries ALL
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_SOURCE_DIR}/nixja"
    COMMAND ${CMAKE_COMMAND} -E create_hardlink
        "$<TARGET_FILE:nixja>" "${CMAKE_SOURCE_DIR}/nixja"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_SOURCE_DIR}/nixja-gen"
    COMMAND ${CMAKE_COMMAND} -E create_hardlink
        "$<TARGET_FILE:nixja-gen>" "${CMAKE_SOURCE_DIR}/nixja-gen"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_SOURCE_DIR}/nixja-scan-deps"
    COMMAND ${CMAKE_COMMAND} -E create_hardlink
        "$<TARGET_FILE:nixja-scan-deps>" "${CMAKE_SOURCE_DIR}/nixja-scan-deps"
    DEPENDS nixja nixja-gen nixja-scan-deps
    VERBATIM
)
