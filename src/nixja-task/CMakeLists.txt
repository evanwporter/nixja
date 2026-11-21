add_executable(nixja-task ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

target_link_libraries(nixja-task PRIVATE
    nixja-common
    nlohmann_json::nlohmann_json
    PkgConfig::NIX_STORE
    fmt::fmt
    spdlog::spdlog
    argparse::argparse
    expected_fs::expected_fs
)

target_precompile_headers(nixja-task PRIVATE
    <nlohmann/json.hpp>
    <nix/main/shared.hh>
    <nix/store/derivations.hh>
    <nix/store/globals.hh>
    <nix/store/store-api.hh>
    <nix/store/store-dir-config.hh>
    <nix/store/store-open.hh>
    <nix/util/source-accessor.hh>
)
