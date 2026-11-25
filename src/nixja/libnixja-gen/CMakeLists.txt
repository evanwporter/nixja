add_library(libnixja-gen STATIC nixja_gen.cpp)
set_target_properties(libnixja-gen PROPERTIES OUTPUT_NAME nixja-gen)

target_compile_options(libnixja-gen PRIVATE -Uunix)
target_link_libraries(libnixja-gen PUBLIC
    nixja-common
    libninja
    libninja-re2c
    clangDriver
    clangBasic
    clangTooling
    LLVMSupport
)
target_include_directories(libnixja-gen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/external/ninja/src
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)
target_precompile_headers(libnixja-gen PRIVATE
    <nlohmann/json.hpp>
    <nix/main/shared.hh>
    <nix/store/derivations.hh>
    <nix/store/globals.hh>
    <nix/store/store-api.hh>
    <nix/store/store-dir-config.hh>
    <nix/store/store-open.hh>
    <nix/util/source-accessor.hh>
)
target_compile_definitions(libnixja-gen PRIVATE
    NIXJA_SCAN_DEPS_PATH="${nixjaScanDeps}"
    NIXJA_TASK_PATH="${nixjaTask}"
    NIX_STORE_PACKAGE_PATH="${NIX_STORE_PACKAGE_PATH}"
    NIX_UTIL_PACKAGE_PATH="${NIX_UTIL_PACKAGE_PATH}"
    FMT_PACKAGE_PATH="${FMT_PACKAGE_PATH}"
    SPDLOG_PACKAGE_PATH="${SPDLOG_PACKAGE_PATH}"
    CC_PACKAGE_PATH="${CC_PACKAGE_PATH}"
    CLANG_PACKAGE_PATH="${CLANG_PACKAGE_PATH}"
    LLVM_PACKAGE_PATH="${LLVM_PACKAGE_PATH}"
)
