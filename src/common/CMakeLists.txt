add_library(nixja-common STATIC common.cpp)

target_link_libraries(nixja-common PUBLIC
    PkgConfig::NIX_STORE
    nlohmann_json::nlohmann_json
)

target_include_directories(nixja-common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
