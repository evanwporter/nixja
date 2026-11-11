add_library(nixja-common STATIC common.cpp)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/toolchain.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/toolchain.hpp"
    @ONLY
)

target_sources(nixja-common PUBLIC
    "${NIXJA_TOOLCHAIN_HEADER}"
)

target_link_libraries(nixja-common PUBLIC
    PkgConfig::NIX_STORE
    nlohmann_json::nlohmann_json
    fmt::fmt
)

target_include_directories(nixja-common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_precompile_headers(nixja-common PRIVATE
    <nlohmann/json.hpp>
    <nix/main/shared.hh>
    <nix/store/derivations.hh>
    <nix/store/globals.hh>
    <nix/store/store-api.hh>
    <nix/store/store-dir-config.hh>
    <nix/store/store-open.hh>
    <nix/util/posix-source-accessor.hh>
)
