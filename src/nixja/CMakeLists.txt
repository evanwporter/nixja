add_executable(nixja ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

target_link_libraries(nixja PUBLIC
    nixja-common
    fmt::fmt
    PkgConfig::NIX_STORE
    nlohmann_json::nlohmann_json
    argparse::argparse
)

target_precompile_headers(nixja PRIVATE
    <nlohmann/json.hpp>
    <nix/main/shared.hh>
    <nix/store/derivations.hh>
    <nix/store/globals.hh>
    <nix/store/store-api.hh>
    <nix/store/store-dir-config.hh>
    <nix/store/store-open.hh>
    <nix/util/posix-source-accessor.hh>
)

target_compile_definitions(nixja PRIVATE
    NIXJA_VERSION="${NIXJA_VERSION}"
    NIXJA_GEN_PATH="${nixjaGen}"
    NIX_STORE_PACKAGE_PATH="${NIX_STORE_PACKAGE_PATH}"
    NIX_UTIL_PACKAGE_PATH="${NIX_UTIL_PACKAGE_PATH}"
    FMT_PACKAGE_PATH="${FMT_PACKAGE_PATH}"
    LLVM_PACKAGE_PATH="${LLVM_PACKAGE_PATH}"
)
