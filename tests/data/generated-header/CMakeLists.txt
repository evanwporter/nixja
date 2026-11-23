cmake_minimum_required(VERSION 3.20)
project(example_generated_header LANGUAGES CXX)

set(generated_header "${CMAKE_CURRENT_BINARY_DIR}/generated.h")
add_custom_command(
    OUTPUT "${generated_header}"
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/generate.sh" "${generated_header}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generate.sh"
    VERBATIM
)

add_executable(main main.cpp "${generated_header}")
target_include_directories(main PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
