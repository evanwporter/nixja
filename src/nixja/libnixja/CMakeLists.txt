add_library(libnixja STATIC nixja.cpp)
set_target_properties(libnixja PROPERTIES OUTPUT_NAME nixja)

target_link_libraries(libnixja PUBLIC libnixja-gen)
target_include_directories(libnixja PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(libnixja PRIVATE -Uunix)
