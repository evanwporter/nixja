add_executable(nixja-scan-deps ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

target_link_libraries(nixja-scan-deps PRIVATE
    pantor::inja
)

target_precompile_headers(nixja-scan-deps PRIVATE
    <inja/inja.hpp>
    <nlohmann/json.hpp>
)

target_include_directories(nixja-scan-deps PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR})

file(READ
    "${CMAKE_CURRENT_SOURCE_DIR}/nixja-scan-deps.nix.j2"
    NIXJA_SCAN_DEPS_TEMPLATE
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/nixja-scan-deps.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/nixja-scan-deps.hpp"
    @ONLY
)
