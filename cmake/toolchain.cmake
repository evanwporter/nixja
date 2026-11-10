# Discover the tools and package outputs used by generated derivations.
# FYI: This is the only human written line in this file

find_program(BASH_PROGRAM bash REQUIRED)
find_program(CP_PROGRAM cp REQUIRED)
find_program(CC_PROGRAM cc REQUIRED)
find_program(CLANG_SCAN_DEPS_PROGRAM clang-scan-deps REQUIRED)

# Local builds use the binaries staged in the project directory. Packaged
# builds supply absolute store paths for the tools used inside derivations.
set(nixjaGen "./nixja-gen" CACHE STRING "Path to nixja-gen")
set(nixjaScanDeps "./nixja-scan-deps" CACHE STRING "Path to nixja-scan-deps")

function(nixja_store_item_from_program program output_prefix)
    get_filename_component(${output_prefix}_BIN_DIR "${program}" DIRECTORY)
    get_filename_component(${output_prefix}_STORE_PATH
        "${${output_prefix}_BIN_DIR}" DIRECTORY)
    get_filename_component(${output_prefix}_STORE_NAME
        "${${output_prefix}_STORE_PATH}" NAME)

    set(${output_prefix}_BIN_DIR "${${output_prefix}_BIN_DIR}" PARENT_SCOPE)
    set(${output_prefix}_STORE_NAME "${${output_prefix}_STORE_NAME}" PARENT_SCOPE)
endfunction()

nixja_store_item_from_program("${BASH_PROGRAM}" BASH)
nixja_store_item_from_program("${CP_PROGRAM}" COREUTILS)
nixja_store_item_from_program("${CC_PROGRAM}" CC)
nixja_store_item_from_program("${CLANG_SCAN_DEPS_PROGRAM}" CLANG_TOOLS)

find_package(PkgConfig REQUIRED)
pkg_check_modules(NIX_STORE REQUIRED IMPORTED_TARGET nix-store)

find_library(NIX_STORE_SHARED_LIBRARY
    NAMES nixstore
    HINTS ${NIX_STORE_LIBRARY_DIRS}
    NO_DEFAULT_PATH
    REQUIRED)
find_library(NIX_UTIL_SHARED_LIBRARY
    NAMES nixutil
    HINTS ${NIX_STORE_LIBRARY_DIRS}
    NO_DEFAULT_PATH
    REQUIRED)

get_filename_component(NIX_STORE_SHARED_LIBRARY_REALPATH
    "${NIX_STORE_SHARED_LIBRARY}" REALPATH)
get_filename_component(NIX_UTIL_SHARED_LIBRARY_REALPATH
    "${NIX_UTIL_SHARED_LIBRARY}" REALPATH)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

get_target_property(FMT_SHARED_LIBRARY fmt::fmt IMPORTED_LOCATION_RELEASE)
if(NOT FMT_SHARED_LIBRARY)
    get_target_property(FMT_SHARED_LIBRARY fmt::fmt IMPORTED_LOCATION)
endif()
if(NOT FMT_SHARED_LIBRARY)
    message(FATAL_ERROR "Could not determine the shared library for fmt::fmt")
endif()
get_filename_component(FMT_SHARED_LIBRARY_REALPATH
    "${FMT_SHARED_LIBRARY}" REALPATH)

function(nixja_store_path_from_path path output_variable)
    string(REGEX MATCH "^/nix/store/[^/]+" store_path "${path}")
    if("${store_path}" STREQUAL "")
        message(FATAL_ERROR "Expected a Nix store path, got: ${path}")
    endif()
    set(${output_variable} "${store_path}" PARENT_SCOPE)
endfunction()

foreach(tool IN ITEMS nixjaGen nixjaScanDeps)
    if(IS_ABSOLUTE "${${tool}}")
        nixja_store_path_from_path("${${tool}}" ${tool}_STORE_PATH)
        get_filename_component(${tool}_STORE_NAME "${${tool}_STORE_PATH}" NAME)
    endif()
endforeach()

nixja_store_path_from_path("${NIX_STORE_SHARED_LIBRARY_REALPATH}" NIX_STORE_PACKAGE_PATH)
nixja_store_path_from_path("${NIX_UTIL_SHARED_LIBRARY_REALPATH}" NIX_UTIL_PACKAGE_PATH)
nixja_store_path_from_path("${FMT_SHARED_LIBRARY_REALPATH}" FMT_PACKAGE_PATH)
nixja_store_path_from_path("${nlohmann_json_DIR}" NLOHMANN_JSON_PACKAGE_PATH)
