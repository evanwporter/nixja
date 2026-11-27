#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <nix/store/content-address.hh>
#include <nix/store/store-api.hh>
#include <nix/util/source-accessor.hh>

#include <fmt/format.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/StringSaver.h>
#include <nlohmann/json.hpp>

#include "ClangScanDeps.hpp"
#include "common.hpp"
#include "compilation_database.hpp"
#include "dependency_parser.hpp"
#include "logging.hpp"

namespace fs = std::filesystem;

using json = nlohmann::json;

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
    const std::vector<std::string>& dependencies,
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

// clang-scan-deps needs absolute working directories, which are only known
// after the source tree has been reconstructed in the builder.
Result<void> nixja_scan_deps(const fs::path& compilation_database) {
    try {
        const fs::path source_root = fs::current_path();
        std::ifstream input(compilation_database);
        if (!input)
            throw std::runtime_error("Cannot open compilation database");
        json database = json::parse(input);
        if (!database.is_array() || database.empty())
            throw std::runtime_error("Compilation database must be a non-empty array");
        spdlog::info("Scanning dependencies for {} compilation commands", database.size());
        spdlog::debug("Compilation database: {}; source root: {}", compilation_database.string(), source_root.string());

        const fs::path configured_task_path = NIXJA_TASK_PATH;
        const fs::path task_path = configured_task_path.is_absolute()
            ? configured_task_path
            : source_root / configured_task_path;
        for (auto& entry : database) {
            fs::path directory = entry.value("nixja_directory", entry.at("directory").get<std::string>());
            if (directory.is_relative())
                directory = source_root / directory;
            entry["directory"] = directory.lexically_normal().string();
        }

        // Adapt the database in memory for a single clang-scan-deps invocation.
        struct TempFile {
            const char* name;
            ~TempFile() { unlink(name); }
        };
        json scan_database = database;
        {
            for (auto& entry : scan_database) {
                // Force a unique dependency target for each entry, even if
                // the original command specifies its own -MT or -MQ.
                llvm::BumpPtrAllocator allocator;
                llvm::StringSaver saver(allocator);
                llvm::SmallVector<const char*> args;
                llvm::cl::TokenizeGNUCommandLine(entry.at("command").get<std::string>(), saver, args);
                if (args.empty())
                    return Err("Compilation command is empty");

                // HACK: This is super hacky
                // Ideally we set C_INCLUDE_PATH & CXX_INCLUDE_PATH in a wrapper script
                // Clang's in-process driver does not run Nix compiler wrappers.
                // Supply the wrapper's implicit headers and resource directory.
                const fs::path compiler_name = fs::path(args.front()).filename();
                for (const auto& item : entry.value("nixja_inputs", json::array())) {
                    const fs::path wrapper = item.get<std::string>();
                    if (!fs::exists(wrapper / "bin" / compiler_name))
                        continue;
                    for (const auto* name : { "cc-cflags", "libc-cflags", "libcxx-cxxflags" }) {
                        std::ifstream flags_file(wrapper / "nix-support" / name);
                        if (!flags_file)
                            continue;
                        const std::string flags((std::istreambuf_iterator<char>(flags_file)), { });
                        llvm::SmallVector<const char*> wrapper_args;
                        llvm::cl::TokenizeGNUCommandLine(flags, saver, wrapper_args);
                        for (const char* arg : wrapper_args)
                            args.push_back(arg);
                    }
                    break;
                }

                entry.erase("command");
                entry["arguments"] = std::move(args);
            }
        }
        const auto scan_results = clang_scan_deps(
            std::make_unique<NixjaCompilationDatabase>(scan_database));

        if (scan_results.size() != database.size())
            return Err("Unexpected number of dependency scan results");

        spdlog::debug(
            "Scanned dependencies for {} compilation commands",
            scan_results.size());

        nix::experimentalFeatureSettings.experimentalFeatures.set(
            "ca-derivations dynamic-derivations");
        nix::initLibStore(true);
        const nix::ref<nix::Store> store = nix::openStore();
        const fs::path canonical_task = fs::canonical(task_path);
        const fs::path task_command = store->isInStore(canonical_task.string())
            ? canonical_task
            : fs::path(store->printStorePath(store->addToStore(
                  nixja_store_name("nixja-task", "nixja-scan-deps"),
                  nix::makeFSSourceAccessor(canonical_task),
                  nix::ContentAddressMethod::Raw::NixArchive)));
        const auto [task_store_path, ignored] = store->toStorePath(task_command.string());

        for (const auto& [entry, result] :
             std::views::zip(database, scan_results)) {

            const fs::path directory = entry.at("directory").get<std::string>();

            const fs::path relative = directory.lexically_relative(source_root);

            if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
                return Err("Compilation directory is outside the source tree");

            const std::string target = entry.at("output").get<std::string>();

            if (result.output != target)
                return Err("Dependency result mismatch for " + target);

            fs::current_path(directory);

            const auto deps = local_dependencies(
                result.dependencies, store, source_root);

            fs::current_path(source_root);

            spdlog::debug("Target {}: {} local dependencies, {} store dependencies", target, deps.local.size(), deps.store.size());
            const auto files = add_dependencies_to_store(deps.local, store, source_root, directory);
            TaskSpec task {
                .inputs = files,
                .build_relative = relative,
                .command = entry.at("command").get<std::string>(),
            };
            for (const auto& item : entry.at("nixja_outputs"))
                task.outputs.emplace_back(item.get<std::string>());
            std::vector<std::string> inputSrcs;
            for (const auto& file : files)
                inputSrcs.push_back(file.source_store_path.filename().string());
            inputSrcs.push_back(fs::path(store->printStorePath(task_store_path)).filename().string());

            for (const auto& path : {
                     NIX_STORE_PACKAGE_PATH,
                     NIX_UTIL_PACKAGE_PATH,
                     FMT_PACKAGE_PATH,
                     SPDLOG_PACKAGE_PATH,
                     CC_PACKAGE_PATH })
                inputSrcs.push_back(fs::path(path).filename().string());

            for (const auto& path : deps.store)
                inputSrcs.push_back(fs::path(path).filename().string());

            for (const auto& item : entry.value("nixja_inputs", json::array())) {
                const std::string path = item.get<std::string>();
                if (store->isInStore(path)) {
                    const auto [root, ignored_subpath] = store->toStorePath(path);
                    inputSrcs.push_back(fs::path(store->printStorePath(root)).filename().string());
                }
            }

            const std::string name = entry.at("nixja_output").get<std::string>();
            const auto drv = TRY(writeDerivation(store, {
                                                            .name = name,
                                                            .storeNamePrefix = "nixja-scan-deps",
                                                            .buildPhase = make_task_command(task, task_command),
                                                            .outputMethod = "nar",
                                                            .inputSrcs = inputSrcs,
                                                        }));

            const char* destination = std::getenv(name.c_str());
            if (!destination)
                return Err("Missing Nix output variable: " + name);

            fs::copy_file(store->printStorePath(drv), destination);
            spdlog::debug("Wrote derivation for {} to {}", target, destination);
        }
        spdlog::info("Generated {} compilation derivations", database.size());
        return { };
    } catch (const std::exception& error) {
        return Err(error.what());
    }
}

int main(int argc, char* argv[]) {
    init_logging("nixja-scan-deps", spdlog::level::debug);
    if (argc != 2) {
        std::cerr << "Usage: nixja-scan-deps COMPILATION_DATABASE\n";
        return 1;
    }

    const auto result = nixja_scan_deps(argv[1]);

    if (!result.has_value()) {
        spdlog::error(result.error());
        return 1;
    }
}
