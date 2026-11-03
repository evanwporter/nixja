add_executable(nixja-gen ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

target_compile_options(nixja-gen PRIVATE -Uunix)

target_link_libraries(nixja-gen PRIVATE
    libninja
    libninja-re2c
    pantor::inja
    PkgConfig::NIX_STORE
)

target_precompile_headers(nixja-gen PRIVATE
    <inja/inja.hpp>
    <nlohmann/json.hpp>
)

target_include_directories(nixja-gen PRIVATE
    ${CMAKE_SOURCE_DIR}/external/ninja/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

file(READ
    "${CMAKE_CURRENT_SOURCE_DIR}/ninja-gen.nix.j2"
    NIXJA_GEN_TEMPLATE
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/nixja-gen.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/nixja-gen.hpp"
    @ONLY
)
