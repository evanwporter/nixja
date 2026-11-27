#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <clang/Tooling/CompilationDatabase.h>
#include <nlohmann/json.hpp>

class NixjaCompilationDatabase : public clang::tooling::CompilationDatabase {
public:
    explicit NixjaCompilationDatabase(const nlohmann::json& database) {
        for (const auto& entry : database) {
            commands_.emplace_back(
                entry.at("directory").get<std::string>(),
                entry.at("file").get<std::string>(),
                entry.at("arguments").get<std::vector<std::string>>(),
                entry.value("output", std::string()));
        }
    }

    std::vector<clang::tooling::CompileCommand> getCompileCommands(llvm::StringRef file) const override {
        std::vector<clang::tooling::CompileCommand> result;
        for (const auto& command : commands_) {
            const auto path = (std::filesystem::path(command.Directory) / command.Filename).lexically_normal();
            if (path == std::filesystem::path(file.str()).lexically_normal())
                result.push_back(command);
        }
        return result;
    }

    std::vector<std::string> getAllFiles() const override {
        std::vector<std::string> files;
        for (const auto& command : commands_)
            files.push_back((std::filesystem::path(command.Directory) / command.Filename).lexically_normal().string());
        return files;
    }

    std::vector<clang::tooling::CompileCommand> getAllCompileCommands() const override {
        return commands_;
    }

private:
    std::vector<clang::tooling::CompileCommand> commands_;
};
