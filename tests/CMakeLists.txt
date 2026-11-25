find_package(GTest REQUIRED)

add_executable(nixja-tests
    ${CMAKE_CURRENT_SOURCE_DIR}/test_compile_command.cpp
)

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
