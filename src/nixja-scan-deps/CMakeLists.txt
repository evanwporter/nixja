add_executable(nixja-scan-deps ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

find_program(NIXJA_LLVM_TABLEGEN llvm-tblgen HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Opts.inc"
    COMMAND "${NIXJA_LLVM_TABLEGEN}" -gen-opt-parser-defs
        -I "${LLVM_INCLUDE_DIRS}"
        "${CMAKE_SOURCE_DIR}/external/clang-scan-deps/Opts.td"
        -o "${CMAKE_CURRENT_BINARY_DIR}/Opts.inc"
    DEPENDS "${CMAKE_SOURCE_DIR}/external/clang-scan-deps/Opts.td"
    VERBATIM
)
add_library(clang-scan-deps-lib STATIC
    "${CMAKE_SOURCE_DIR}/external/clang-scan-deps/ClangScanDeps.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/Opts.inc"
)
target_include_directories(clang-scan-deps-lib
    PUBLIC "${CMAKE_SOURCE_DIR}/external/clang-scan-deps"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_link_libraries(clang-scan-deps-lib PUBLIC clang-cpp LLVM)
target_include_directories(nixja-scan-deps PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

target_link_libraries(nixja-scan-deps PRIVATE
    nixja-common
    nlohmann_json::nlohmann_json
    PkgConfig::NIX_STORE
    fmt::fmt
    spdlog::spdlog
    LLVMSupport
    clang-scan-deps-lib
)

target_compile_definitions(nixja-scan-deps PRIVATE
    NIXJA_TASK_PATH="${nixjaTask}"
    NIX_STORE_PACKAGE_PATH="${NIX_STORE_PACKAGE_PATH}"
    NIX_UTIL_PACKAGE_PATH="${NIX_UTIL_PACKAGE_PATH}"
    FMT_PACKAGE_PATH="${FMT_PACKAGE_PATH}"
    SPDLOG_PACKAGE_PATH="${SPDLOG_PACKAGE_PATH}"
    CC_PACKAGE_PATH="${CC_PACKAGE_PATH}"
)

target_precompile_headers(nixja-scan-deps PRIVATE
    <nlohmann/json.hpp>
    <nix/main/shared.hh>
    <nix/store/derivations.hh>
    <nix/store/globals.hh>
    <nix/store/store-api.hh>
    <nix/store/store-dir-config.hh>
    <nix/store/store-open.hh>
    <nix/util/source-accessor.hh>
)
