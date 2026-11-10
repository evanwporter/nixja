add_executable(nixja ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

target_link_libraries(nixja PUBLIC
    fmt::fmt
    PkgConfig::NIX_STORE
    nlohmann_json::nlohmann_json
)
