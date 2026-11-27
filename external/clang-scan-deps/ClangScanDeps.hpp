#pragma once

#include <string>
#include <memory>

namespace clang::tooling { class CompilationDatabase; }

int clang_scan_deps(std::unique_ptr<clang::tooling::CompilationDatabase> compilation_database, const std::string& output_file);
