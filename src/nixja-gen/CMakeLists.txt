add_executable(nixja-gen ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

target_compile_options(nixja-gen PRIVATE -Uunix)

target_compile_definitions(nixja-gen PRIVATE
    NIXJA_SCAN_DEPS_PATH="${nixjaScanDeps}"
    NIX_STORE_PACKAGE_PATH="${NIX_STORE_PACKAGE_PATH}"
    NIX_UTIL_PACKAGE_PATH="${NIX_UTIL_PACKAGE_PATH}"
    FMT_PACKAGE_PATH="${FMT_PACKAGE_PATH}"
)

target_link_libraries(nixja-gen PRIVATE
    nixja-common
    libninja
    libninja-re2c
    PkgConfig::NIX_STORE
    nlohmann_json::nlohmann_json
    fmt::fmt
)

target_precompile_headers(nixja-gen PRIVATE
    <nlohmann/json.hpp>
    <nix/main/shared.hh>
    <nix/store/derivations.hh>
    <nix/store/globals.hh>
    <nix/store/store-api.hh>
    <nix/store/store-dir-config.hh>
    <nix/store/store-open.hh>
    <nix/util/posix-source-accessor.hh>
)

target_include_directories(nixja-gen PRIVATE
    ${CMAKE_SOURCE_DIR}/external/ninja/src
)
