find_program(NIXJA_LLVM_TABLEGEN
    llvm-tblgen HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Opts.inc"
    COMMAND "${NIXJA_LLVM_TABLEGEN}" -gen-opt-parser-defs
        -I "${LLVM_INCLUDE_DIRS}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Opts.td"
        -o "${CMAKE_CURRENT_BINARY_DIR}/Opts.inc"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Opts.td"
    VERBATIM
)

add_library(clang-scan-deps-lib STATIC ClangScanDeps.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/Opts.inc")

target_include_directories(clang-scan-deps-lib
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${LLVM_INCLUDE_DIRS}
        ${CLANG_INCLUDE_DIRS})

target_link_libraries(clang-scan-deps-lib PUBLIC clang-cpp LLVM)
