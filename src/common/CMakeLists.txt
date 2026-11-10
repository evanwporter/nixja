add_library(nixja-common STATIC common.cpp)

set(NIXJA_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(NIXJA_TOOLCHAIN_HEADER "${NIXJA_GENERATED_INCLUDE_DIR}/toolchain.hpp")
file(MAKE_DIRECTORY "${NIXJA_GENERATED_INCLUDE_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/toolchain.hpp.in"
    "${NIXJA_TOOLCHAIN_HEADER}"
    @ONLY
)

target_sources(nixja-common PUBLIC
    "${NIXJA_TOOLCHAIN_HEADER}"
)

target_link_libraries(nixja-common PUBLIC
    PkgConfig::NIX_STORE
    nlohmann_json::nlohmann_json
)

target_include_directories(nixja-common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${NIXJA_GENERATED_INCLUDE_DIR}
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
