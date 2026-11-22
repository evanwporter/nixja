#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <argparse/argparse.hpp>
#include <expected_fs/expected_fs.hpp>

#include "common.hpp"

namespace fs = expected_fs;

Result<StoreFile> parseStoreFile(const std::string& input) {
    const auto pos = input.find(':');

    if (pos == std::string::npos)
        return Err("Invalid input format: {}", input);

    const fs::path store_path = input.substr(0, pos);
    const fs::path relative_path = input.substr(pos + 1);

    if (store_path.empty() || relative_path.empty() || relative_path.is_absolute())
        return Err("Invalid store file: {}", input);

    return StoreFile {
        .source_store_path = store_path,
        .file_relative = relative_path,
    };
}

// TODO: This can be made simpler

/**
 * @brief Links all the necessary files from the nix store to the path under $NIX_BUILD_TOP.
 *
 * @param inputs List of inputs parsed from the command line args.
 * @param nix_build_top Absolute path of $NIX_BUILD_TOP
 * @return Success or an error if derivation creation fails.
 */
Result<void> linkPhase(const std::vector<StoreFile>& inputs, const fs::path& nix_build_top, const fs::path& build_relative) {
    TRY_EC(fs::create_directories(nix_build_top));
    for (const auto& store_file : inputs) {
        const auto destination = nix_build_top / store_file.file_relative;
        TRY_EC(fs::create_directories(destination.parent_path()));
        // Inputs produced by an earlier edge replace a source-tree symlink at
        // the same location.
        TRY_EC(fs::remove(destination));
        TRY_EC(fs::create_symlink(store_file.source_store_path, destination));
    }

    TRY_EC(fs::create_directories(nix_build_top / build_relative));

    return { };
}

/// Creates a mirror of all the directories and symlinks the files.
Result<void> linkSourceTree(
    const fs::path& input_directory,
    const fs::path& nix_build_top) {
    TRY_EC(fs::create_directories(nix_build_top));
    for (const auto& entry : fs::recursive_directory_iterator(input_directory)) {
        const fs::path relative = entry.path().lexically_relative(input_directory);
        const fs::path destination = nix_build_top / relative;
        if (entry.is_directory())
            TRY_EC(fs::create_directories(destination));
        else
            TRY_EC(fs::create_symlink(entry.path(), destination));
    }

    return { };
}

Result<void> buildPhase(const std::vector<StoreFile>& inputs, const std::vector<fs::path> outputs, const fs::path& nix_build_top, const fs::path& build_relative, const std::string& command) {
    // cd into the build directory
    TRY_EC(fs::current_path(nix_build_top / build_relative));

    // Create the output directories
    for (const auto& output : outputs) {
        const fs::path parent = fs::path(output).parent_path();
        if (!parent.empty())
            TRY_EC(fs::create_directories(parent));
    }

    const pid_t child = fork();
    if (child == -1)
        return Err("[nixja-task] Could not start command: {}", command);
    if (child == 0) {
        execlp("bash", "bash", "-e", "-c", command.c_str(), nullptr);
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return Err("[nixja-task] Command failed: {}", command);

    return { };
}

Result<void> installPhase(const std::vector<fs::path> outputs) {
    const char* out_env = std::getenv("out");
    if (!out_env)
        return Err("[nixja-task] out is not set");
    const fs::path out = out_env;

    TRY_EC(fs::create_directories(out));

    for (const auto& output : outputs) {
        const fs::path destination = out / output;
        TRY_EC(fs::create_directories(destination.parent_path()));
        TRY_EC(fs::copy_file(output, destination));
    }

    return { };
}

Result<void> nixja_task(
    const std::vector<std::string>& inputs,
    const std::vector<fs::path>& outputs,
    const std::optional<fs::path>& input_directory,
    const fs::path& build_relative,
    const std::string& command) {

    const char* nix_build_top_env = std::getenv("NIX_BUILD_TOP");
    if (!nix_build_top_env)
        return Err("[nixja-task] NIX_BUILD_TOP is not set. Are you in a derivation?\n");
    const fs::path nix_build_top = nix_build_top_env;

    std::vector<StoreFile> store_files;
    store_files.reserve(inputs.size());

    for (const auto& input : inputs)
        store_files.push_back(TRY(parseStoreFile(input)));

    if (input_directory)
        TRY(linkSourceTree(*input_directory, nix_build_top));
    TRY(linkPhase(store_files, nix_build_top, build_relative));
    TRY(buildPhase(store_files, outputs, nix_build_top, build_relative, command));
    TRY(installPhase(outputs));

    return { };
}

int main(int argc, char* argv[]) {

    /// Combine following block with nixja-scan-deps
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::optional<fs::path> input_directory;

    std::vector<std::string>* current_list = nullptr;

    /// nixja-scan-deps starts in the reconstructed project source tree.
    const auto source_root_result = fs::current_path();
    if (!source_root_result) {
        std::cerr << "Could not get current directory: " << source_root_result.error().message() << '\n';
        return 1;
    }
    const fs::path source_root = *source_root_result;

    /// The Ninja working directory relative to project_root.
    fs::path build_relative = ".";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--")
            break;
        else if (arg == "-i") {
            current_list = &inputs;
        } else if (arg == "-o") {
            current_list = &outputs;
        } else if (arg == "-d") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for -d\n";
                return 1;
            }
            input_directory = argv[++i];
        } else if (arg == "-C") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for -C\n";
                return 1;
            }
            build_relative = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown flag: " << arg << '\n';
            return 1;
        } else if (current_list) {
            current_list->push_back(arg);
        } else {
            std::cerr << "Expected -i or -o before: " << arg << '\n';
            return 1;
        }
    }

    const std::string command = arguments_after_separator(argc, argv);

    std::vector<fs::path> output_paths;
    output_paths.reserve(outputs.size());

    for (const auto& output : outputs)
        output_paths.emplace_back(output);

    const auto result = nixja_task(
        inputs,
        output_paths,
        input_directory,
        build_relative,
        command);

    if (!result) {
        std::cerr << result.error() << '\n';
        return 1;
    }

    return 0;
}
