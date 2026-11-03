add_executable(nixja-scan-deps ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

target_link_libraries(nixja-scan-deps PRIVATE
    nlohmann_json::nlohmann_json
    PkgConfig::NIX_STORE
)

target_precompile_headers(nixja-scan-deps PRIVATE
    <nlohmann/json.hpp>
)

target_include_directories(nixja-scan-deps PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(nixja-scan-deps PRIVATE
    fmt::fmt
)
