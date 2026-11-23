cmake_minimum_required(VERSION 3.20)
project(example_run_script LANGUAGES CXX)

set(generated_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated.cpp")
add_custom_command(
    OUTPUT "${generated_cpp}"
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/gen.sh"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen.sh"
    VERBATIM
)

add_executable(main main.cpp "${generated_cpp}")
