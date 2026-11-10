add_executable(nixja-scan-deps ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

target_link_libraries(nixja-scan-deps PRIVATE
    nixja-common
    nlohmann_json::nlohmann_json
    PkgConfig::NIX_STORE
    fmt::fmt
)

target_precompile_headers(nixja-scan-deps PRIVATE
    <nlohmann/json.hpp>
    <nix/main/shared.hh>
    <nix/store/derivations.hh>
    <nix/store/globals.hh>
    <nix/store/store-api.hh>
    <nix/store/store-dir-config.hh>
    <nix/store/store-open.hh>
    <nix/util/posix-source-accessor.hh>
)
