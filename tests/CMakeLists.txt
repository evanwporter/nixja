find_package(GTest REQUIRED)

add_executable(nixja-tests
    ${CMAKE_CURRENT_SOURCE_DIR}/test_compile_command.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_dependency_parser.cpp
)

target_include_directories(nixja-tests PRIVATE ${CMAKE_SOURCE_DIR}/src/nixja-scan-deps)

target_link_libraries(nixja-tests PRIVATE
    libnixja-gen
    GTest::gtest_main
    clangTooling
    clangDriver
    clangFrontend
    clangBasic
    LLVMSupport
    fmt::fmt
)

include(GoogleTest)
gtest_discover_tests(nixja-tests)
