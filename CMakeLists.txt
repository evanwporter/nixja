cmake_minimum_required(VERSION 3.20)

project(nixja LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NINJA_BUILD_BINARY OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory(external/ninja)

add_executable(nixja
    src/main.cpp
)

target_include_directories(nixja PRIVATE
    third_party/ninja/src
)

target_link_libraries(nixja PRIVATE
    libninja
    libninja-re2c
)

install(TARGETS nixja RUNTIME DESTINATION bin)
