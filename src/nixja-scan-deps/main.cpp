#include <algorithm>
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

#include <nix/store/content-address.hh>
#include <nix/store/store-api.hh>
#include <nix/util/source-accessor.hh>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common.hpp"

namespace fs = std::filesystem;

using json = nlohmann::json;

/// Parse Makefile dependency output from clang-scan-deps
/// TODO: Use whatever ninja uses internally
/// TODO: Can makefile_content be a reference instead?
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
        const size_t start = line.find_first_not_of(" \t\r");

        if (start == std::string::npos || line[start] == '#')
            continue;

        const size_t colon = line.find(':');

        if (colon == std::string::npos)
            continue;

        std::string target = line.substr(start, colon - start);

        const size_t end = target.find_last_not_of(" \t\r");
        target.erase(end + 1);

        const std::string dependencies = line.substr(colon + 1);

        std::istringstream dep_stream(dependencies);
        std::string dependency;

        while (dep_stream >> dependency)
            dependency_graph[target].insert(dependency);

        dependency_graph.try_emplace(target);
    }

    return dependency_graph;
}

struct Dependencies {
    /// A list of source and header files located under the `project_root`
    /// These paths need to be added to the store
    std::unordered_set<std::string> local;

    /// A list of paths inside the nix store. These paths are already in
    /// in the store
    std::unordered_set<std::string> store;
};

/// Filter out dependencies outside the source root
/// TODO: Rename since we are no longer obtaining only local deps
Dependencies local_dependencies(
    const std::unordered_set<std::string>& dependencies,
    const nix::ref<nix::Store> store,
    const fs::path& project_root) {

    Dependencies result;

    for (const auto& dependency : dependencies) {
        const fs::path path(dependency);

        // Check if path is in the nix store
        // TODO: Allow alternative locations other than /nix/store
        if (dependency.starts_with("/nix/store/")) {
            // TODO: I can do this by just slicing everything after the first level of the nix store path
            const auto [store_path, sub_path] = store->toStorePath(dependency);
            result.store.insert(store->printStorePath(store_path));
            continue;
        }

        // Scanner paths are relative to the Ninja build directory, but
        // containment must be checked against the original project root.
        const fs::path absolute = fs::absolute(path).lexically_normal();
        const fs::path relative = absolute.lexically_relative(project_root);

        // Check if the path is absolute
        if (relative.empty() || relative.is_absolute())
            continue;

        // Check if its outside the project_root (where nixja-scan-deps is called from)
        if (relative.begin() != relative.end() && *relative.begin() == "..")
            continue;

        result.local.insert(absolute.generic_string());
    }

    return result;
}

/// Scan compiler dependencies for a single edge
/// TODO: This needs to be done in the derivation on a per EDGE basis
std::unordered_set<std::string> scan_dependencies(const std::string command) {
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

    return all_dependencies;
}

int main(int argc, char* argv[]) {
    /// Inputs to the specific ninja edge
    /// Currently unused since all this information can be gathered from the
    /// scan command.
    std::vector<std::string> inputs;

    /// Outputs of the ninja edge
    std::vector<std::string> outputs;

    std::vector<std::string>* current_list = nullptr;

    /// nixja-scan-deps starts in the reconstructed project source tree.
    const fs::path source_root = fs::current_path();

    const fs::path configured_task_path = NIXJA_TASK_PATH;
    const fs::path task_path = configured_task_path.is_absolute()
        ? configured_task_path
        : source_root / configured_task_path;

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

    // The compiler command, and clang-scan-deps, use Ninja's working
    // directory just as a normal `ninja -C` invocation would.
    fs::current_path(source_root / build_relative);

    nix::initLibStore(true);
    const nix::ref<nix::Store> store = nix::openStore();

    const fs::path task_command = [&] {
        const fs::path canonical_task_path = fs::canonical(task_path);
        if (store->isInStore(canonical_task_path.string())) {
            return canonical_task_path;
        }

        const auto stored = store->addToStore(
            nixja_store_name("nixja-task", "nixja-scan-deps"),
            nix::makeFSSourceAccessor(canonical_task_path),
            nix::ContentAddressMethod::Raw::Flat);
        return fs::path(store->printStorePath(stored));
    }();

    const auto dependencies = local_dependencies(scan_dependencies(command), store, source_root);

    /// TODO: Maybe give a error or something here?
    if (outputs.empty())
        return 0;

    std::string name = outputs.front();

    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '.', '_');

    nix::experimentalFeatureSettings.experimentalFeatures.set(
        "ca-derivations dynamic-derivations");

    /// [Absolute /nix/store/XYZ-path-filename, Relative path under source_root]
    std::vector<StoreFile> store_paths;

    // TODO: I use this pattern quite a bit I think
    for (const auto& dependency : dependencies.local) {
        const fs::path absolute = fs::absolute(dependency).lexically_normal();
        const fs::path file_relative = absolute.lexically_relative(source_root);

        // Every dependency path must exist, because we aren't checking for errors here
        const fs::path file = fs::canonical(absolute);

        const auto stored = store->addToStore(
            nixja_store_name(file.filename().string(), "nixja-scan-deps"),
            nix::makeFSSourceAccessor(file),
            nix::ContentAddressMethod::Raw::Flat);

        store_paths.emplace_back(store->printStorePath(stored), file_relative);
    }

    TaskSpec task_spec {
        .inputs = store_paths,
        .build_dir = build_relative,
        .command = command,
    };

    for (const auto& output : outputs)
        task_spec.outputs.emplace_back(output);

    /// Add each storePath as a inputSrc
    std::vector<std::string> inputSrcs;
    inputSrcs.reserve(store_paths.size() + 1);
    for (const auto& [storePath, file_relative] : store_paths)
        inputSrcs.push_back(fs::path(storePath).filename().string());

    const auto [task_store_path, _] = store->toStorePath(task_command.string());
    inputSrcs.push_back(fs::path(store->printStorePath(task_store_path)).filename().string());
    inputSrcs.push_back(fs::path(NIX_STORE_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(NIX_UTIL_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(FMT_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(SPDLOG_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(CC_PACKAGE_PATH).filename().string());

    for (const auto& storePath : dependencies.store)
        inputSrcs.push_back(fs::path(storePath).filename().string());

    const json inputDrvs = json::object();

    const auto path = writeDerivation(store, {
                                                 .name = name,
                                                 .storeNamePrefix = "nixja-scan-deps",
                                                 .buildPhase = make_task_command(task_spec, task_command),
                                                 .outputMethod = "nar",
                                                 .inputSrcs = inputSrcs,
                                                 .inputDrvs = inputDrvs,
                                             });

    if (!path.has_value()) {
        std::cerr << path.error() << std::endl;
        return 1;
    }

    std::cout << store->printStorePath(path.value()) << '\n';

    return 0;
}
