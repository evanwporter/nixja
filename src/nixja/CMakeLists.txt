add_subdirectory(libnixja-gen)
add_subdirectory(libnixja)

add_executable(nixja main.cpp)

target_link_libraries(nixja PRIVATE libnixja argparse::argparse)
target_compile_definitions(nixja PRIVATE NIXJA_VERSION="${NIXJA_VERSION}")
