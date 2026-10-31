add_executable(nixja-gen ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)

target_link_libraries(nixja-gen PRIVATE
    libninja
    libninja-re2c
    pantor::inja
)

target_include_directories(nixja-gen PRIVATE
    ${CMAKE_SOURCE_DIR}/external/ninja/src
)

