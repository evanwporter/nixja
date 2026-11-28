#include <clang/Frontend/CompilerInstance.h>

#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/TargetParser/Host.h>

#include <gtest/gtest.h>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "nixja_gen.hpp"

class CompileCommandTest : public ::testing::Test {
protected:
    clang::DiagnosticOptions opts;
    llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnostics = clang::CompilerInstance::createDiagnostics(
        *llvm::vfs::getRealFileSystem(), opts, new clang::IgnoringDiagConsumer());
    clang::driver::Driver driver { "clang++",
                                   llvm::sys::getDefaultTargetTriple(),
                                   *diagnostics };
};

TEST_F(CompileCommandTest, ReturnsTrueForCompilationCommands) {
    const std::vector<std::vector<const char*>> cases {
        { "clang++", "-c", "main.cpp", "-o", "main.o" },
        { "clang++", "main.cpp", "-o", "main" },
        { "clang", "-c", "main.c", "-o", "main.o" },
        { "clang++", "-c", "-O2", "-g", "-Wall", "-Iinclude", "-DDEBUG", "main.cpp" },
        { "clang++", "-x", "c++", "-c", "source", "-o", "source.o" },
        { "clang++", "-c", "-fPIC", "-std=c++23", "main.cpp" },
        { "clang++", "-S", "main.cpp" },
        { "clang++", "-E", "main.cpp" },
        { "clang++", "-fsyntax-only", "main.cpp" },
        { "clang++", "-c", "main.cpp", "-MD", "-MF", "main.d" },
    };

    for (const auto& args : cases) {
        diagnostics->Reset();
        EXPECT_TRUE(is_compile_command(driver, args)) << fmt::format(
            "{}",
            fmt::join(args, " "));
    }
}

TEST_F(CompileCommandTest, ReturnsFalseForNonCompilationCommands) {
    const std::vector<std::vector<const char*>> cases {
        { "python", "script.py" },
        { "python3", "-m", "module" },
        { "./it" },
        { "./it", "input.cpp" },
        { "echo", "hello" },
        { "clang++", "main.o", "-o", "main" },
        { "clang++", "-shared", "main.o", "-o", "libmain.so" },
        { "clang++", "-c", "main.s", "-o", "main.o" },
        { "clang++", "--version" },
        { "clang++", "--help" },
        { "clang++", "-print-search-dirs" },
        { "clang++" },
        { "clang++", "-c", "-invalid-nixja-test-option", "main.cpp" },
    };

    for (const auto& args : cases) {
        diagnostics->Reset();

        llvm::outs().flush();
        testing::internal::CaptureStdout();

        const bool result = is_compile_command(driver, args);

        llvm::outs().flush();
        testing::internal::GetCapturedStdout();

        EXPECT_FALSE(result) << fmt::format(
            "{}",
            fmt::join(args, " "));
    }
}
