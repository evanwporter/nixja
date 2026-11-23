cmake_minimum_required(VERSION 3.20)
project(example_hello LANGUAGES CXX)

add_executable(hello main.cpp)
target_compile_features(hello PRIVATE cxx_std_14)
