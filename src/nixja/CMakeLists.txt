add_executable(nixja ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

file(READ
    "${CMAKE_CURRENT_SOURCE_DIR}/nixja.nix.j2"
    NIXJA_TEMPLATE
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/nixja.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/nixja.hpp"
    @ONLY
)

target_include_directories(nixja PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)
