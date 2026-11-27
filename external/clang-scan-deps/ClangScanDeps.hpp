#pragma once

#include <string>

int clang_scan_deps(const std::string& compilation_database, const std::string& output_file);
