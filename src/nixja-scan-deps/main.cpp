#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <nix/store/store-api.hh>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common.hpp"

namespace fs = std::filesystem;

using json = nlohmann::json;

/// Parse Makefile dependency output from clang-scan-deps
/// TODO: Use whatever ninja uses internally
std::unordered_map<std::string, std::unordered_set<std::string>>
parse_makefile(std::string makefile_content) {
    std::unordered_map<std::string, std::unordered_set<std::string>>
        dependency_graph;

    makefile_content = std::regex_replace(
        makefile_content,
        std::regex(R"(\\\r?\n)"),
        " ");

    std::istringstream file_stream(makefile_content);
    std::string line;

    while (std::getline(file_stream, line)) {
        size_t start = line.find_first_not_of(" \t\r");

        if (start == std::string::npos || line[start] == '#')
            continue;

        size_t colon = line.find(':');

        if (colon == std::string::npos)
            continue;

        std::string target = line.substr(start, colon - start);

        size_t end = target.find_last_not_of(" \t\r");
        target.erase(end + 1);

        std::string dependencies = line.substr(colon + 1);

        std::istringstream dep_stream(dependencies);
        std::string dependency;

        while (dep_stream >> dependency)
            dependency_graph[target].insert(dependency);

        dependency_graph.try_emplace(target);
    }

    return dependency_graph;
}

/// Filter out dependencies outside the source root
std::unordered_set<std::string>
local_dependencies(
    const std::unordered_set<std::string>& dependencies) {
    const fs::path source_root = fs::current_path();

    std::unordered_set<std::string> local;

    for (const auto& dependency : dependencies) {
        const fs::path path(dependency);
        const fs::path relative = path.is_absolute()
            ? path.lexically_relative(source_root)
            : path.lexically_normal();

        if (relative.empty() || relative.is_absolute())
            continue;

        if (relative.begin() != relative.end() && *relative.begin() == "..")
            continue;

        local.insert(relative.generic_string());
    }

    return local;
}

/// Scan compiler dependencies for a single edge
/// TODO: This needs to be done in the derivation on a per EDGE basis
std::unordered_set<std::string>
scan_dependencies(const std::string command) {
    // Run clang-scan-deps.
    const std::string scan_command = "clang-scan-deps -- " + command;

    const std::string output = exec(scan_command.c_str());

    const auto dependency_map = parse_makefile(output);

    /// TODO: Think about whether we need to keep the dependencies seperate

    // Combine all the dependencies into a single set
    std::unordered_set<std::string> all_dependencies;
    for (auto& [target, dependencies] : dependency_map) {
        all_dependencies.insert(dependencies.begin(), dependencies.end());
    }

    // Remove all dependencies that point outside of the root path
    // all_dependencies = local_dependencies(all_dependencies);

    return all_dependencies;
}

/// TODO: Combine more efficiently with other cmd line parsing
std::string arguments_after_separator(int argc, char* argv[]) {
    std::ostringstream result;
    bool after_separator = false;

    for (int i = 1; i < argc; ++i) {
        if (!after_separator) {
            after_separator = std::string_view(argv[i]) == "--";
            continue;
        }

        if (result.tellp() > 0)
            result << ' ';
        result << argv[i];
    }

    return result.str();
}

int main(int argc, char* argv[]) {
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;

    std::vector<std::string>* current_list = nullptr;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--")
            break;
        else if (arg == "-i") {
            current_list = &inputs;
        } else if (arg == "-o") {
            current_list = &outputs;
        }
        /// TODO: Use .startWith when I upgrade my C++ version
        else if (!arg.empty() && arg[0] == '-') {
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

    const auto dependencies = scan_dependencies(command);

    /// TODO: Maybe give a error or something here?
    if (outputs.empty())
        return 0;

    std::string name = outputs.front();

    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '.', '_');

    const fs::path project_root = fs::current_path();
    nix::experimentalFeatureSettings.experimentalFeatures.set(
        "ca-derivations dynamic-derivations");

    nix::initLibStore(true);
    const nix::ref<nix::Store> store = nix::openStore();
    std::unordered_map<std::string, std::string> store_paths;

    for (const auto& dependency : dependencies) {
        const fs::path file = fs::weakly_canonical(dependency);

        const auto stored = store->addToStore(
            nixja_store_name(file.filename().string()),
            nix::PosixSourceAccessor::createAtRoot(file),
            nix::ContentAddressMethod::Raw::Flat);

        store_paths.emplace(
            dependency,
            store->printStorePath(stored));
    }

    std::string linkPhase = "mkdir -p \"$NIX_BUILD_TOP\"; ";
    for (const auto [dep, store_path] : store_paths) {
        const fs::path parent = fs::path(dep).parent_path();
        if (!parent.empty())
            linkPhase += fmt::format(
                "mkdir -p \"$NIX_BUILD_TOP/{}\"; ", parent.generic_string());
        linkPhase += fmt::format("ln -s {} \"$NIX_BUILD_TOP/{}\"; ", store_path, dep);
    };

    std::string installPhase = "mkdir -p $out; ";
    for (const auto output : outputs) {
        installPhase += fmt::format(
            "cp {} $out/{}; ", output, fs::path(output).filename().string());
    }

    std::vector<std::string> inputSrcs;
    inputSrcs.reserve(store_paths.size());
    for (const auto& [dependency, storePath] : store_paths)
        inputSrcs.push_back(fs::path(storePath).filename().string());

    const json inputDrvs = json::object();

    const auto derivation = emitDerivation(store, {
                                                      .name = name,
                                                      .system = "x86_64-linux",
                                                      .linkPhase = linkPhase,
                                                      .buildPhase = command,
                                                      .installPhase = installPhase,
                                                      .outputMethod = "nar",
                                                      .inputSrcs = inputSrcs,
                                                      .inputDrvs = inputDrvs,
                                                  });

    if (!derivation.has_value()) {
        std::cerr << derivation.error() << std::endl;
        return 1;
    }

    std::cout << derivation.value();

    return 0;
}
