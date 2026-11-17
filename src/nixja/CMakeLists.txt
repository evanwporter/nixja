add_executable(nixja
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/nixja_gen.cpp
)

target_compile_options(nixja PRIVATE -Uunix)

target_link_libraries(nixja PUBLIC
    nixja-common
    fmt::fmt
    spdlog::spdlog
    PkgConfig::NIX_STORE
    nlohmann_json::nlohmann_json
    argparse::argparse
    libninja
    libninja-re2c
    clangDriver
    clangBasic
    LLVMSupport
)

target_precompile_headers(nixja PRIVATE
    <nlohmann/json.hpp>
    <nix/main/shared.hh>
    <nix/store/derivations.hh>
    <nix/store/globals.hh>
    <nix/store/store-api.hh>
    <nix/store/store-dir-config.hh>
    <nix/store/store-open.hh>
    <nix/util/source-accessor.hh>
)

target_compile_definitions(nixja PRIVATE
    NIXJA_VERSION="${NIXJA_VERSION}"
    NIXJA_SCAN_DEPS_PATH="${nixjaScanDeps}"
    NIX_STORE_PACKAGE_PATH="${NIX_STORE_PACKAGE_PATH}"
    NIX_UTIL_PACKAGE_PATH="${NIX_UTIL_PACKAGE_PATH}"
    FMT_PACKAGE_PATH="${FMT_PACKAGE_PATH}"
    SPDLOG_PACKAGE_PATH="${SPDLOG_PACKAGE_PATH}"
    CC_PACKAGE_PATH="${CC_PACKAGE_PATH}"
    CLANG_PACKAGE_PATH="${CLANG_PACKAGE_PATH}"
    LLVM_PACKAGE_PATH="${LLVM_PACKAGE_PATH}"
)

target_include_directories(nixja PRIVATE
    ${CMAKE_SOURCE_DIR}/external/ninja/src
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)
