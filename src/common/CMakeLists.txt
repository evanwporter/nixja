add_library(nixja-common STATIC common.cpp)

target_link_libraries(nixja-common PUBLIC
    PkgConfig::NIX_STORE
    nlohmann_json::nlohmann_json
    fmt::fmt
)

target_include_directories(nixja-common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
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
