#pragma once

#include <memory>
#include <string>
#include <vector>

namespace clang::tooling {
    class CompilationDatabase;
}

struct ScanResult {
    std::string input;
    std::string output;
    std::vector<std::string> dependencies;
};

std::vector<ScanResult> clang_scan_deps(std::unique_ptr<clang::tooling::CompilationDatabase> compilation_database);
