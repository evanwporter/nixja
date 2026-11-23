cmake_minimum_required(VERSION 3.20)
project(example_dynamic_deps LANGUAGES CXX)

set(generated_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated.cpp")
add_custom_command(
    OUTPUT "${generated_cpp}"
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/generate.sh"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generate.sh"
    VERBATIM
)

add_executable(main main.cpp "${generated_cpp}")
target_include_directories(main PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
