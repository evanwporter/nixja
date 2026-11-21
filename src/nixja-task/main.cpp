#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <argparse/argparse.hpp>
#include <expected_fs/expected_fs.hpp>

#include "common.hpp"

namespace fs = expected_fs;

/**
 * @brief Links all the necessary files from the nix store to the path under $NIX_BUILD_TOP.
 *
 * @param inputs List of inputs parsed from the command line args.
 * @param nix_build_top Absolute path of $NIX_BUILD_TOP
 * @return Success or an error if derivation creation fails.
 */
Result<void> linkPhase(const std::vector<StoreFile>& inputs, const fs::path& nix_build_top) {
    TRY_EC(fs::create_directories(nix_build_top));
    for (const auto& store_file : inputs) {
        const auto destination = nix_build_top / store_file.file_relative;
        TRY_EC(fs::create_directories(destination.parent_path()));
        TRY_EC(fs::create_symlink(store_file.source_store_path, destination));
    }

    return { };
}

Result<void> linkPhase(const std::string input_directory) {
}

Result<void> nixja_task() {

    return { };
}

int main(int argc, char* argv[]) {

    /// Combine following block with nixja-scan-deps
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;

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

    return 0;
}
