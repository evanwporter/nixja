add_executable(nixja-task
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/elf.cpp)

target_link_libraries(nixja-task PRIVATE
    nixja-common
    nlohmann_json::nlohmann_json
    fmt::fmt
    expected_fs::expected_fs
    libpatchelf
)

target_precompile_headers(nixja-task PRIVATE
    <nlohmann/json.hpp>
)
