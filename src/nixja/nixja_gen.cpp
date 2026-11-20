#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>
#include <nix/store/store-api.hh>
#include <nix/util/source-accessor.hh>
#include <nlohmann/json.hpp>

#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Job.h>
#include <clang/Driver/Tool.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/StringSaver.h>
#include <llvm/TargetParser/Host.h>

#include "common.hpp"
#include "nixja_gen.hpp"

#include "disk_interface.h"
#include "graph.h"
#include "manifest_parser.h"
#include "state.h"

namespace fs = std::filesystem;

using json = nlohmann::json;

// TODO: Clarify how each field is used
// TODO: Check if I need the full thing
struct Derivation {

    // const nix::StorePath wrapper_drv =
    //     store->parseStorePath("/nix/store/<hash>-main_o.drv.drv");
    //
    // const auto generated_drv =
    //     nix::DownstreamPlaceholder::unknownCaOutput(wrapper_drv, "out");
    //
    // const std::string main_o_out =
    //     nix::DownstreamPlaceholder::unknownDerivation(generated_drv, "out")
    //         .render();

    bool generates_drv;

    /// Store-file name, used in serialized derivation input maps.
    std::string derivation_path;

    /// Absolute store path, used when rendering Nix expressions.
    std::string derivation_store_path;

    /// If `generates_drv` is true then this is not empty
    /// The output of this creats the output path.
    /// This comes from derivation_path
    /// TODO: currently unused
    std::string generated_path;

    /// If `generates_drv` is true then this comes from the output of
    /// generated_path. Otherwise this comes from the output of derivation_path.
    std::string output_path;
};

struct Command {
    /// The command to run
    std::string command;

    /// A set of every input src (into the nix store) that this command requires
    std::unordered_set<std::string> inputSrcs;
};

/// This function parses a compilation command using the LLVM TOKENIZER.
/// It returns a list of the include paths and the library paths.
/// TODO: Check if paths exist
std::unordered_set<std::string> parseCompileCommand(const std::string& command) {

    /// Parse the command into a vector of char*
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver saver(allocator);
    llvm::SmallVector<const char*> args;
    llvm::cl::TokenizeGNUCommandLine(command, saver, args);

    std::unordered_set<std::string> paths;

    // i=1 here to skip the first arg (the command)
    for (size_t i = 1; i < args.size(); ++i) {
        llvm::StringRef arg(args[i]);
        // Handle `-I <path>`
        if (arg == "-I" && i + 1 < args.size())
            paths.emplace(args[++i]);

        // Handle `-I<path>`
        else if (arg.starts_with("-I") && arg.size() > 2)
            paths.emplace(arg.drop_front(2).str());

        // Handle `-L <path>`
        else if (arg == "-L" && i + 1 < args.size())
            paths.emplace(args[++i]);

        // Handle `-L<path>`
        else if (arg.starts_with("-L") && arg.size() > 2)
            paths.emplace(arg.drop_front(2).str());
    }

    return paths;
}

/// Given a generic NINJA command, we parse all the possible NIX STORE paths from it
/// TODO: Parse the complex shell syntax like program1 && program2
/// TODO: Rework to flow better & DRY
Result<Command> parseCommand(
    const std::string& command,
    const fs::path& source_root,
    const std::string& source_store_path,
    const nix::ref<nix::Store> store) {

    // Extract executable from command.
    // TODO: This will break if the file path has spaces
    const auto end = command.find_first_of(" \t\n\r");
    const std::string program = command.substr(0, end);

    // An empty program means its likely a PHONY command
    if (program.empty())
        return Command { .command = command, .inputSrcs = { } };

    const fs::path programPath(program);

    Command result {
        .command = command,
        .inputSrcs = parseCompileCommand(command),
    };

    if (programPath.is_absolute()) {

        // Absolute executable in the nix store, e.g. /nix/store/.../bin/clang++
        if (store->isInStore(programPath.string())) {
            const auto [storePath, subPath] = store->toStorePath(programPath.string());
            result.inputSrcs.insert(store->printStorePath(storePath));
        }

        else {
            const fs::path path_relative = programPath.lexically_relative(source_root);

            if (path_relative.empty() || path_relative.is_absolute() || *path_relative.begin() == "..")
                return Err(
                    "The program ({}) must be either in the nix store or a part of the source directory.",
                    /// TODO: Investigate whether I can get a string_view instead
                    programPath.string());

            // Convert the absolute path to a relative one
            // NOTE: Would it be better too provide a path in the nix store?
            result.command.replace(0, program.size(), (fs::path(source_store_path) / path_relative).string());
        }
    } else if (programPath.has_parent_path()) {
        // Relative executable, e.g. ./scripts/generate or bin/tool

        const fs::path root = fs::canonical(source_root);
        const fs::path resolved = fs::weakly_canonical(root / programPath);

        const fs::path relative = resolved.lexically_relative(root);

        if (relative.empty() || relative.is_absolute())
            return Err("Executable cannot be resolved inside source root");

        // TODO: I think we only need to check the first component
        // but verify this with lexically_relative
        for (const auto& component : relative) {
            if (component == "..")
                return Err(fmt::format("Executable escapes source root: {}", program));
        }

        // Relative executable is already included in the source tree.
        // No additional Nix store input is needed.

    } else {
        // Bare executable name, e.g. clang++, gcc, python3
        // TODO: Eventually we'll need c

        // TODO: Perhaps rework parse_which to return a nix store path directly?
        const fs::path path = TRY(parse_which(program, store));
        const auto [storePath, subPath] = store->toStorePath(path.string());
        result.inputSrcs.insert(store->printStorePath(storePath));
    }

    return result;
}

/// Determine whether a command uses a depfile
/// TODO: Use ninja internals to figure this out
bool has_depfile(const std::string& command) {
    std::istringstream stream(command);
    std::string arg;

    while (stream >> arg) {
        if (arg == "-MF")
            return true;

        if (arg.compare(0, 3, "-MF") == 0 && arg.size() > 3)
            return true;
    }

    return false;
}

/**
 * @brief Creates a Nix derivation for a Ninja build edge.
 *
 * @param edge The Ninja build edge to process.
 * @param spec The initial specification for the Nix derivation.
 * @param generates_drv Whether the derivation generates another derivation.
 * @param source_root The directory where `nixja` was invoked.
 * @param source_store_path The source root's path in the Nix store.
 * @param build_relative The build directory relative to the source root.
 * @param store The Nix store used to create the derivation.
 * @param[out] edge_map Maps Ninja edge IDs to their generated derivations.
 * @return Success or an error if derivation creation fails.
 */
Result<void> create_derivation(
    const Edge* edge,
    const DerivationSpec& spec,
    const bool generates_drv,
    const fs::path& source_root,
    const std::string& source_store_path,
    const fs::path& build_relative,
    const nix::ref<nix::Store> store,
    std::vector<Derivation>& edge_map) {

    // processing a edge means creating a drv file for it

    // TODO: Consider replacing find with its absolute path?
    std::string linkPhase = fmt::format(
        "mkdir -p \"$NIX_BUILD_TOP\"; "
        "(cd {0}; "
        /// Visit every directory and creates the same directory tree
        /// under $NIX_BUILD_TOP.Those directories are real and writable.
        "find . -type d -exec mkdir -p \"$NIX_BUILD_TOP/{{}}\" \\; ; "
        /// Visits every non directory—regular files and symlinks and creates
        /// a symlink at the matching path under $NIX_BUILD_TOP, pointing back
        /// to the immutable source-store file.
        "find . ! -type d -exec ln -s \"{0}/{{}}\" \"$NIX_BUILD_TOP/{{}}\" \\;); ",
        source_store_path);

    /// `collector` obtains the entire closure of nodes
    InputsCollector collector;
    collector.VisitNode(edge->outputs_.front());

    for (const Node* input : collector.inputs()) {
        const fs::path path = input->path();

        /// Checking if the path is absolute filters out /nix/store paths
        if (path.is_absolute())
            continue;

        const Edge* producer = input->in_edge();

        // producer being null means this is a leaf node / source file
        if (producer == nullptr)
            continue;

        const fs::path destination = (build_relative / path).lexically_normal();
        const std::string nix_reference = edge_map[producer->id_].output_path + "/" + path.generic_string();
        linkPhase += fmt::format(
            // create all the directories up to destination path
            "mkdir -p \"$NIX_BUILD_TOP/{}\"; "
            // -sfn ensures that ln doesn't fail if the destination already exists
            "ln -sfn \"{}\" \"$NIX_BUILD_TOP/{}\"; ",
            destination.parent_path().string(),
            nix_reference,
            destination.string());
    }

    // This means that we are not calling `nixja-scan-deps`. Rather we are performing the
    // operation ourself so we cd into build directory
    if (!generates_drv)
        linkPhase += fmt::format(
            "cd \"$NIX_BUILD_TOP/{}\"; ",
            build_relative.generic_string());

    const fs::path nixja_scan_deps_path = NIXJA_SCAN_DEPS_PATH;

    // Collect source inputs.
    std::vector<std::string> inputSrcs;

    inputSrcs.push_back(
        fs::path(source_store_path).filename().string());

    if (nixja_scan_deps_path.is_absolute())
        inputSrcs.push_back(nixja_scan_deps_path.parent_path().parent_path().filename().string());
    inputSrcs.push_back(fs::path(NIX_STORE_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(NIX_UTIL_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(FMT_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(SPDLOG_PACKAGE_PATH).filename().string());

    const auto command = TRY(parseCommand(edge->EvaluateCommand(), source_root, source_store_path, store));

    // TODO: Turn this into actual logging
    //
    // std::cout << "Include directories:\n";
    // for (const auto& path : searchPaths.include) {
    //     const auto [store_path, sub_path] = store->toStorePath(path);
    //     const std::string root = store->printStorePath(store_path) + "/";
    //     std::cout << "  " << root << '\n';
    // }
    //
    // std::cout << "Library directories:\n";
    // for (const auto& path : searchPaths.library) {
    //     const auto [store_path, sub_path] = store->toStorePath(path);
    //     const std::string root = store->printStorePath(store_path) + "/";
    //
    //     std::cout << "  " << root << '\n';
    // }

    for (const auto& path : command.inputSrcs) {
        // Check if path is in the nix store
        if (store->isInStore(path)) {

            // If it is in the nix store then convert it to the store path.
            // storePath: is the /nix/store/XXXXXX
            // subPath: is the the /nix/store/XXXXX/YYYYYY
            const auto [storePath, subPath] = store->toStorePath(path);
            const std::string root = store->printStorePath(storePath);
            inputSrcs.push_back(fs::path(root).filename().string());
        }
    }

    // Collect input derivations.
    json inputDrvs = json::object();

    /// TODO: Can we combine this loop with the one above
    for (const auto node : collector.inputs()) {
        if (const Edge* producer = node->in_edge()) {

            const auto& derivation = edge_map[producer->id_];

            const json dynOutputs = derivation.generates_drv
                ? json::object({
                      { "out", {
                                   { "dynamicOutputs", json::object() },
                                   { "outputs", json::array({ "out" }) },
                               } },
                  })
                : json::object();

            inputDrvs[derivation.derivation_path] = {
                { "dynamicOutputs", dynOutputs },
                { "outputs", json::array({ "out" }) },
            };

        } else {
            // // This node is an already existing source path
            // // so we add it to inputSrcs
            // inputSrcs.push_back(node->path());
            // volatile bool node_is_mapped = edge_map.count(node) != 0;
        }
    }

    auto completeSpec = spec;
    if (!generates_drv)
        completeSpec.buildPhase = command.command;
    completeSpec.linkPhase = linkPhase;
    completeSpec.inputSrcs = inputSrcs;
    completeSpec.inputDrvs = inputDrvs;
    const auto drv_path = TRY(writeDerivation(store, completeSpec));

    // Resolve output paths.
    std::string generated_path;
    std::string output_path;

    const auto ca_output = nix::DownstreamPlaceholder::unknownCaOutput(drv_path, "out");

    if (generates_drv) {
        generated_path = ca_output.render();

        output_path = nix::DownstreamPlaceholder::unknownDerivation(
                          ca_output, "out")
                          .render();
    } else {
        output_path = ca_output.render();
    }

    // Record derivation.
    edge_map[edge->id_] = Derivation {
        .generates_drv = generates_drv,
        .derivation_path = std::string(drv_path.to_string()),
        .derivation_store_path = store->printStorePath(drv_path),
        .output_path = output_path,
    };

    return { };
};

Result<void> process_standard_edge(
    const Edge* edge,
    const fs::path& source_root,
    const std::string& source_store_path,
    const fs::path& build_relative,
    const nix::ref<nix::Store> store,
    std::vector<Derivation>& edge_map) {
    // processing a edge means creating a drv file for it

    // Determine derivation name.
    const auto name = fs::path(edge->outputs_.front()->path()).filename().string();

    std::string derivationName = name;
    std::replace(
        derivationName.begin(),
        derivationName.end(),
        '.',
        '_');

    // Construct build phase.
    const std::string buildPhase = edge->EvaluateCommand();

    // Construct install phase.
    std::string installPhase = "mkdir -p \"$out\"; ";

    for (const auto& output : edge->outputs_) {
        const fs::path parent = fs::path(output->path()).parent_path();
        if (!parent.empty())
            installPhase += fmt::format("mkdir -p \"$out/{0}\"; ", parent.generic_string());
        installPhase += fmt::format(
            "cp '{0}' \"$out/{0}\"; ",
            output->path());
    }

    // Configure derivation.
    const DerivationSpec spec {
        .name = derivationName,
        .storeNamePrefix = "nixja-gen",
        .buildPhase = buildPhase,
        .installPhase = installPhase,
        .outputMethod = "nar",
    };

    TRY(create_derivation(edge, spec, false, source_root, source_store_path, build_relative, store, edge_map));

    return { };
}

Result<void> process_dep_edge(
    const Edge* edge,
    const fs::path& source_root,
    const std::string& source_store_path,
    const fs::path& build_relative,
    const nix::ref<nix::Store> store,
    std::vector<Derivation>& edge_map) {

    // Determine derivation name.
    const auto name = fs::path(edge->outputs_.front()->path()).filename().string();

    std::string derivationName = name;
    std::replace(derivationName.begin(), derivationName.end(), '.', '_');

    // A derivation-producing output must end in .drv.
    derivationName += ".drv";

    // Construct command.
    const std::string command = edge->EvaluateCommand();

    // Construct scanner output arguments.
    std::string outputs_str;

    for (const auto& output : edge->outputs_)
        outputs_str += output->path();

    const std::string buildPhase = fmt::format(
        "NIXJA_SCAN_DEPS=$({} -C '{}' "
        "-i TODO -o {} -- {}); "
        "cp \"$NIXJA_SCAN_DEPS\" \"$out\"",
        NIXJA_SCAN_DEPS_PATH,
        build_relative.generic_string(),
        outputs_str,
        command);

    // Configure derivation.
    const DerivationSpec spec {
        .name = derivationName,
        .storeNamePrefix = "nixja-gen",
        .buildPhase = buildPhase,
        .installPhase = "",
        .outputMethod = "text",
    };

    TRY(create_derivation(edge, spec, true, source_root, source_store_path, build_relative, store, edge_map));

    return { };
}

/// Kahns Algorithm
/// See LC Course Scheduling
/// TODO: only process target closure
Result<void> topo_process(
    const State& state,
    const nix::ref<nix::Store> store,
    std::vector<Derivation>& edge_map,
    const fs::path& source_root,
    const fs::path& build_relative) {
    /// The number of unprocessed dependencies for a give edge
    std::vector<size_t> indegree(state.edges_.size());

    /// Adjaceny list
    /// Maps each edge to all the edges that depend on it (waiting on this edge to be processed)
    std::vector<std::vector<const Edge*>> consumers(state.edges_.size());

    /// Ready Queue
    /// A edge in this queue means that all of its dependencies have been processed
    std::queue<const Edge*> queue;

    // Form the adjaceny list
    for (size_t i = 0; i < state.edges_.size(); i++) {
        const auto edge = state.edges_[i];

        std::unordered_set<size_t> producers;

        for (const auto node : edge->inputs_) {
            /// producer -> consumer
            /// producer outputs a node that consumer needs
            const auto producer = node->in_edge();

            // A nullptr means the node doesn't have a dependency
            if (producer)
                producers.insert(producer->id_);
        }

        indegree[i] = producers.size();

        for (const auto producerId : producers) {
            consumers[producerId].push_back(edge);
        }
    }

    for (size_t i = 0; i < state.edges_.size(); i++) {
        const auto edge = state.edges_[i];
        if (indegree[i] == 0) {
            queue.push(edge);
        }
    }

    /// TODO: Move this out of this function

    const auto stored = store->addToStore(
        nixja_store_name(source_root.filename().string(), "nixja-gen"),
        nix::makeFSSourceAccessor(source_root),
        nix::ContentAddressMethod::Raw::NixArchive,
        nix::HashAlgorithm::SHA256);

    const std::string source_store_path = store->printStorePath(stored);

    while (!queue.empty()) {
        const auto edge = queue.front();
        queue.pop();

        if (has_depfile(edge->EvaluateCommand()))
            TRY(process_dep_edge(edge, source_root, source_store_path, build_relative, store, edge_map));
        else
            TRY(process_standard_edge(edge, source_root, source_store_path, build_relative, store, edge_map));

        for (const auto neighbor : consumers[edge->id_]) {
            indegree[neighbor->id_]--;

            if (indegree[neighbor->id_] == 0)
                queue.push(neighbor);
        }
    }

    return { };
}

/**
 * @brief Generates a Nix derivation for a target in a Ninja build graph.
 *
 * @param target The Ninja build target to generate a derivation for.
 * @param source_root The directory where `nixja` is called from.
 * @param build_dir The directory containing the build.ninja file.
 * @return The Nix store path of the generated derivation
 */
Result<std::string> nixja_gen(const std::string& target, const fs::path& source_root, const fs::path& build_dir) {
    // Load Ninja build graph.
    const fs::path ninja_file = build_dir / "build.ninja";

    State state;
    RealDiskInterface disk;
    ManifestParser parser(&state, &disk);

    std::string error;

    if (!parser.Load(ninja_file.string(), &error))
        return Err(error);

    nix::experimentalFeatureSettings.experimentalFeatures.set(
        "ca-derivations dynamic-derivations");
    nix::initLibStore(true);
    const nix::ref<nix::Store> store = nix::openStore();

    std::vector<Derivation> edge_map(state.edges_.size());
    TRY(topo_process(state, store, edge_map, source_root, build_dir.lexically_relative(source_root)));

    const Node* node = state.LookupNode(target);

    if (node == nullptr) {
        // No such target in the loaded build graph.
        // TODO: error
        return Err("");
    }

    const Edge* target_edge = node->in_edge();

    if (target_edge == nullptr) {
        // The target is a source file or has no producing build edge.
        // TODO: Error
        return Err("");
    }

    const Derivation& target_derivation = edge_map[target_edge->id_];

    /// If we aren't building something that requires a dynamic derivation (and returns a drv)
    /// then we can return early. The only commands that do this are ones that generate a depfile.
    if (!target_derivation.generates_drv)
        return target_derivation.derivation_store_path;

    const auto generated_drv = nix::DownstreamPlaceholder::unknownCaOutput(
        store->parseStorePath(target_derivation.derivation_store_path), "out");
    const auto generated_output = nix::DownstreamPlaceholder::unknownDerivation(
        generated_drv, "out");

    // `target_derivation.derivation_store_path` is a *.drv.drv.
    // We need to unwrap it to obtain the .drv which we then return
    // and pass to nix-build to turn that .drv into a output

    /// This derivation depends on a *.drv.drv file
    const json finalInputDrvs = {
        { target_derivation.derivation_path, {
                                                 { "dynamicOutputs", {
                                                                         { "out", {
                                                                                      { "dynamicOutputs", json::object() },
                                                                                      { "outputs", json::array({ "out" }) },
                                                                                  } },
                                                                     } },
                                                 { "outputs", json::array({ "out" }) },
                                             } },
    };

    /// The final derivation (*.drv)
    const auto final_drv = TRY(writeDerivation(store, {
                                                          .name = target,
                                                          .linkPhase = "",
                                                          .buildPhase = "",
                                                          .installPhase = fmt::format("cp -r \"{}\" \"$out\"", generated_output.render()),
                                                          .outputMethod = "nar",
                                                          .inputSrcs = json::array(),
                                                          .inputDrvs = finalInputDrvs,
                                                      }));
    return store->printStorePath(final_drv);
}
