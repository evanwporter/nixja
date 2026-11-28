#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fmt/ranges.h>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-bash.h>

#include <nix/store/store-api.hh>
#include <nix/util/serialise.hh>
#include <nix/util/source-accessor.hh>

// macro conflict between spdlog and nix
#ifdef debug
#undef debug
#endif

#include <spdlog/spdlog.h>

#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Driver/Action.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Frontend/CompilerInstance.h>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/CommandLine.h>
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

    // TODO: instead of using all this. We should really set the input.drvs
    //
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
    std::string output_name = "out";

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

struct TextEdit {
    uint32_t start;
    uint32_t end;
    std::string replacement;
};

void apply_edits(
    std::string& source,
    std::vector<TextEdit>& edits) {
    std::sort(
        edits.begin(),
        edits.end(),
        [](const auto& a, const auto& b) {
            return a.start > b.start;
        });

    for (const auto& edit : edits) {
        source.replace(
            edit.start,
            edit.end - edit.start,
            edit.replacement);
    }
}

// Dynamic derivation outputs must end in .drv. Keep the Ninja output
// recognizable and include the edge id to prevent sanitized-name collisions.
std::string batch_output_name(const Edge* edge) {
    std::string name = edge->outputs_.front()->path();
    for (char& c : name) {
        const auto ch = static_cast<unsigned char>(c);
        if (!std::isalnum(ch) && c != '_' && c != '-')
            c = '_';
    }
    return fmt::format("output_{}_{}.drv", name, edge->id_);
}

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

bool is_compile_command(clang::driver::Driver& driver, llvm::ArrayRef<const char*> args) {
    if (args.empty() || !args.front())
        return false;

    // The Clang driver interprets argv as compiler arguments regardless of
    // argv[0]; do not mistake arbitrary programs that take source paths for
    // compiler invocations.
    const std::string executable = fs::path(args.front()).filename().string();
    static const std::regex compiler_name(
        R"((?:.+-)?(?:clang(?:\+\+)?|gcc|g\+\+|cc|c\+\+)(?:-[0-9]+(?:\.[0-9]+)*)?)");
    if (!std::regex_match(executable, compiler_name))
        return false;

    driver.setCheckInputsExist(false);

    std::unique_ptr<clang::driver::Compilation> C(
        driver.BuildCompilation(args));

    if (!C || C->containsError())
        return false;
    size_t compiler_jobs = 0;
    for (const auto& job : C->getJobs()) {
        const auto& job_args = job.getArguments();
        if (!job_args.empty() && std::string_view(job_args.front()) == "-cc1")
            ++compiler_jobs;
    }
    return compiler_jobs == 1;
}

std::string_view node_text(TSNode node, std::string_view source) {
    const auto start = ts_node_start_byte(node);
    const auto end = ts_node_end_byte(node);
    return source.substr(start, end - start);
}

void collect_commands(const TSNode node, const std::string_view source, std::vector<CommandInvocation>& commands) {
    if (std::string_view(ts_node_type(node)) == "command") {
        const auto name = ts_node_child_by_field_name(node, "name", 4);

        if (!ts_node_is_null(name)) {
            commands.push_back({
                .program = node_text(name, source),
                .command = node_text(node, source),
                .program_offset = ts_node_start_byte(name),
            });
        }
    }

    for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
        collect_commands(
            ts_node_named_child(node, i),
            source,
            commands);
    }
}

/// Given a generic NINJA command, we parse all the possible NIX STORE paths from it
/// We also rework the program (the first arg in the command), so that it complies.
///
/// For example if the program is an absolute path under source_root, then we add the path
/// to the store and replace the program with the path from the store.
/// TODO: Parse the complex shell syntax like program1 && program2
/// TODO: Rework to flow better & DRY
Result<Command> parseCommand(
    const std::string& command,
    const fs::path& source_root,
    const std::string& source_store_path,
    const nix::ref<nix::Store> store) {

    TSParser* parser = ts_parser_new();

    if (!parser)
        return Err("Failed to create treesitter parser");

    if (!ts_parser_set_language(parser, tree_sitter_bash())) {
        ts_parser_delete(parser);
        return Err("Failed to set bash language in treesitter");
    }

    TSTree* tree = ts_parser_parse_string(
        parser,
        nullptr,
        command.data(),
        command.size());

    if (!tree) {
        ts_parser_delete(parser);
        return Err("Failed to parse bash command");
    }

    const TSNode root = ts_tree_root_node(tree);

    if (ts_node_has_error(root)) {
        ts_tree_delete(tree);
        ts_parser_delete(parser);
        return Err("Invalid bash command");
    }

    std::vector<CommandInvocation> commands;

    collect_commands(root, command, commands);

    // CommandInvocation only references the original command string.
    // We no longer need the tree or parser.
    ts_tree_delete(tree);
    ts_parser_delete(parser);

    Command result {
        .command = command,
        .inputSrcs = { },
    };

    std::vector<TextEdit> edits;

    for (const auto& invocation : commands) {
        const std::string program(invocation.program);
        const fs::path programPath(program);

        // `:` is a no-op
        if (program == ":")
            continue;

        // Collect include and library paths for each command.
        // TODO: Do we need to make sure its a compile command first
        auto paths = parseCompileCommand(
            std::string(invocation.command));

        result.inputSrcs.insert(paths.begin(), paths.end());

        if (programPath.is_absolute()) {

            // Absolute executable already in the Nix store.
            if (store->isInStore(programPath.string())) {
                const auto [storePath, subPath] = store->toStorePath(programPath.string());

                result.inputSrcs.insert(
                    store->printStorePath(storePath));
            }

            else {

                // Absolute executable is not in the nix store, but located at a path under source_root.
                // We need to add it to the nix store.

                const fs::path path_relative = programPath.lexically_relative(source_root);

                // Check if executable is located outside of source_root
                if (path_relative.empty() || path_relative.is_absolute() || *path_relative.begin() == "..")
                    return Err(
                        "The program ({}) must be either in the "
                        "nix store or a part of the source directory.",
                        program);

                const fs::path source_file = fs::canonical(programPath);

                // Add the executable to the store (so we can use it as a input.src)
                const auto stored_file = store->addToStore(
                    nixja_store_name(
                        source_file.filename().string(),
                        "nixja-gen"),
                    nix::makeFSSourceAccessor(source_file),
                    nix::ContentAddressMethod::Raw::NixArchive);

                const std::string store_path = store->printStorePath(stored_file);

                // We need to replace the program relative path with its path in the nix store.
                // Queue the replacement rather than immediately modifying the command.
                edits.push_back({
                    .start = invocation.program_offset,
                    .end = invocation.program_offset + static_cast<uint32_t>(invocation.program.size()),
                    .replacement = store_path,
                });

                result.inputSrcs.insert(store_path);
            }

        } else if (programPath.has_parent_path()) {

            // Relative executable, e.g. ./scripts/generate.
            const fs::path root = fs::canonical(source_root);

            const fs::path resolved = fs::weakly_canonical(root / programPath);

            const fs::path relative = resolved.lexically_relative(root);

            if (relative.empty() || relative.is_absolute())
                return Err(
                    "Executable cannot be resolved inside source root");

            for (const auto& component : relative) {
                if (component == "..")
                    return Err(
                        "Executable escapes source root: {}",
                        program);
            }

            // Relative executable is already included in
            // the source tree.

        } else {

            // Bare executable name, e.g. clang++, gcc, python3
            const fs::path path = TRY(parse_which(program, store));

            const auto [storePath, subPath] = store->toStorePath(path.string());

            result.inputSrcs.insert(
                store->printStorePath(storePath));
        }
    }

    // Apply replacements in descending byte-offset order.
    apply_edits(result.command, edits);

    return result;
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
    std::vector<Derivation>& edge_map,
    const std::vector<const Edge*>& batch = { }) {

    // processing a edge means creating a drv file for it

    const fs::path nixja_scan_deps_path = NIXJA_SCAN_DEPS_PATH;
    const fs::path nixja_task_path = NIXJA_TASK_PATH;

    // Collect source inputs.
    std::vector<std::string> inputSrcs = spec.inputSrcs;

    inputSrcs.push_back(
        fs::path(source_store_path).filename().string());

    if (nixja_scan_deps_path.is_absolute())
        inputSrcs.push_back(nixja_scan_deps_path.parent_path().parent_path().filename().string());
    if (nixja_task_path.is_absolute())
        inputSrcs.push_back(nixja_task_path.parent_path().parent_path().filename().string());
    inputSrcs.push_back(fs::path(NIX_STORE_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(NIX_UTIL_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(FMT_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(SPDLOG_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(CC_PACKAGE_PATH).filename().string());

    const std::vector<const Edge*> edges = batch.empty() ? std::vector<const Edge*> { edge } : batch;
    for (const Edge* current : edges) {
        const auto command = TRY(parseCommand(current->EvaluateCommand(), source_root, source_store_path, store));
        for (const auto& path : command.inputSrcs) {
            if (store->isInStore(path)) {
                const auto [storePath, subPath] = store->toStorePath(path);
                inputSrcs.push_back(fs::path(store->printStorePath(storePath)).filename().string());
            }
        }
    }

    // Collect input derivations.
    json inputDrvs = json::object();

    // Meson lists libraries behind order-only .symbols dependencies, so they are not direct inputs
    // but they still need to be tracked and parsed so nixja-task can find them.
    for (const Edge* current : edges) {
        InputsCollector collector;
        collector.VisitNode(current->outputs_.front());
        for (const Node* node : collector.inputs()) {
            const Edge* producer = node->in_edge();
            if (!producer)
                continue;
            const auto& derivation = edge_map[producer->id_];
            auto& input = inputDrvs[derivation.derivation_path];
            if (input.is_null())
                input = {
                    { "dynamicOutputs", json::object() },
                    { "outputs", json::array() }
                };
            const std::string& output = derivation.output_name;
            if (std::find(input["outputs"].begin(), input["outputs"].end(), output) == input["outputs"].end())
                input["outputs"].push_back(output);
            if (derivation.generates_drv)
                input["dynamicOutputs"][output] = {
                    { "dynamicOutputs", json::object() },
                    { "outputs", json::array({ "out" }) }
                };
        }
    }

    auto completeSpec = spec;
    completeSpec.inputSrcs = inputSrcs;
    completeSpec.inputDrvs = inputDrvs;
    const auto drv_path = TRY(writeDerivation(store, completeSpec));

    for (const Edge* current : edges) {
        /// Name is the output name (by default this is `out`)
        const std::string name = batch.empty() ? "out" : batch_output_name(current);

        /// For static edges this is the final output path. But for dynamic edges this is the derivation path
        /// that build the final output
        const auto ca_output = nix::DownstreamPlaceholder::unknownCaOutput(drv_path, name);

        /// This is the output path of the derivation
        const std::string output_path = generates_drv
            ? nix::DownstreamPlaceholder::unknownDerivation(ca_output, "out").render()
            : ca_output.render();

        /// Create the derivation and add it to the edge map
        edge_map[current->id_] = Derivation {
            .generates_drv = generates_drv,
            .output_name = name,
            .derivation_path = std::string(drv_path.to_string()),
            .derivation_store_path = store->printStorePath(drv_path),
            .generated_path = generates_drv ? ca_output.render() : "",
            .output_path = output_path,
        };
    }

    return { };
};

/// A static edge is one where all dependencies are known prior to build time
Result<void> process_static_edge(
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

    TaskSpec task_spec;

    /// Tracks source files (ie: files that already exist and don't need to be created by
    /// a ninja edge). These files are then added to the store and then inputSrcs. They are
    /// also passed along as inputs to `nixja-task`.
    InputsCollector collector;
    collector.VisitNode(edge->outputs_.front());

    std::vector<const Node*> source_inputs;
    source_inputs.reserve(collector.inputs().size());

    for (const Node* input : collector.inputs()) {
        const Edge* producer = input->in_edge();

        // If this file has no producer then its a source file and
        // needs to be added to the store.
        if (producer == nullptr) {
            source_inputs.push_back(input);
            continue;
        }

        // Otherwise a previous build edge / derivation produced it, and so
        // we need to obtain the output path of the derivation.
        task_spec.inputs.emplace_back(
            fs::path(edge_map[producer->id_].output_path) / input->path(),
            (build_relative / input->path()).lexically_normal());
    }

    // Add every source file to the store and returns a list of StoreFiles
    const auto stored_source_inputs = add_dependencies_to_store(
        source_inputs, store, source_root, source_root / build_relative);

    task_spec.inputs.insert(
        task_spec.inputs.begin(),
        stored_source_inputs.begin(),
        stored_source_inputs.end());

    for (const auto output : edge->outputs_)
        task_spec.outputs.push_back(output->path());
    task_spec.build_relative = build_relative;
    task_spec.command = TRY(parseCommand(
                                edge->EvaluateCommand(), source_root, source_store_path, store))
                            .command;

    /// Obtain working NIXJA_TASK_PATH
    const fs::path configured_task_path = NIXJA_TASK_PATH;
    const fs::path task_path = configured_task_path.is_absolute()
        ? configured_task_path
        : source_root / configured_task_path;
    const fs::path canonical_task_path = fs::canonical(task_path);
    const fs::path task_command = store->isInStore(canonical_task_path.string())
        ? canonical_task_path
        : fs::path(store->printStorePath(store->addToStore(
              nixja_store_name("nixja-task", "nixja-gen"),
              nix::makeFSSourceAccessor(canonical_task_path),
              nix::ContentAddressMethod::Raw::NixArchive)));
    const auto [task_store_path, _] = store->toStorePath(task_command.string());

    const auto buildPhase = make_task_command(task_spec, task_command);

    json inputSrcs = json::array({
        fs::path(store->printStorePath(task_store_path)).filename().string(),
    });
    for (const auto& input : task_spec.inputs) {
        if (!store->isInStore(input.source_store_path.string()))
            continue;

        // TODO: Check if we can skip converting to StorePath
        const auto [store_path, _] = store->toStorePath(input.source_store_path.string());

        // Every path needs to be added to input srcs
        inputSrcs.push_back(fs::path(store->printStorePath(store_path)).filename().string());
    }

    // Configure derivation.
    const DerivationSpec spec {
        .name = derivationName,
        .storeNamePrefix = "nixja-gen",
        .buildPhase = buildPhase,
        .outputMethod = "nar",
        .inputSrcs = std::move(inputSrcs),
    };

    TRY(create_derivation(edge, spec, false, source_root, source_store_path, build_relative, store, edge_map));

    return { };
}

/// A dynamic edge is one where not all dependencies are known at build time. They
/// must be discovered.
Result<void> process_dynamic_edges(
    const std::vector<const Edge*>& edges,
    const fs::path& source_root,
    const std::string& source_store_path,
    const fs::path& build_relative,
    const nix::ref<nix::Store> store,
    std::vector<Derivation>& edge_map) {

    /// Compilation database
    /// This is added to the store, and then passed to nixja-scan-deps as a arg.
    /// Contains a list of every compialtion command / dynamic edge.
    json database = json::array();

    std::vector<std::string> outputs;
    std::string linkPhase = fmt::format(
        "mkdir -p \"$NIX_BUILD_TOP\"; (cd {0}; "
        /// Visit every directory and creates the same directory tree
        /// under $NIX_BUILD_TOP.Those directories are real and writable.
        "find . -type d -exec mkdir -p \"$NIX_BUILD_TOP/{{}}\" \\; ; "
        /// Visits every non directory—regular files and symlinks and creates
        /// a symlink at the matching path under $NIX_BUILD_TOP, pointing back
        /// to the immutable source-store file.
        "find . ! -type d -exec ln -s \"{0}/{{}}\" \"$NIX_BUILD_TOP/{{}}\" \\;); ",
        source_store_path);
    for (const Edge* edge : edges) {
        if (edge->inputs_.empty() || edge->outputs_.empty())
            return Err("Compilation edge {} has no input or output", edge->id_);

        /// The nix derivation produced by the nixja-scan-deps.
        /// Since multiple derivations are produced potentially (one for each dynamic
        /// edge/compilation command)
        const std::string output_name = batch_output_name(edge);
        outputs.push_back(output_name);

        /// Files that the ninja build edge produces when it runs
        json edge_outputs = json::array();
        for (const auto* output : edge->outputs_)
            edge_outputs.push_back(output->path());

        auto parsed = TRY(parseCommand(edge->EvaluateCommand(), source_root, source_store_path, store));
        // CMake emits absolute source-tree paths, which do not exist in the builder.
        const std::string source_prefix = source_root.string() + "/";
        const std::string relative_prefix = fs::path(".").lexically_relative(build_relative).generic_string() + "/";
        size_t position = 0;
        while ((position = parsed.command.find(source_prefix, position)) != std::string::npos) {
            parsed.command.replace(position, source_prefix.size(), relative_prefix);
            position += relative_prefix.size();
        }
        fs::path input_file = edge->inputs_.front()->path();
        if (input_file.is_absolute()) {
            const auto relative = input_file.lexically_relative(source_root);
            if (!relative.empty() && *relative.begin() != "..")
                input_file = fs::path(".").lexically_relative(build_relative) / relative;
        }

        database.push_back({
            { "directory", (source_root / build_relative).lexically_normal().string() },

            /// TODO: Pass this to nixja-scan-deps as -C and get rid of this key
            { "nixja_directory", build_relative.generic_string() },

            /// Inputs found by parsing the command
            { "nixja_inputs", parsed.inputSrcs },
            { "file", input_file.generic_string() },
            { "output", edge->outputs_.front()->path() },
            { "command", parsed.command },
            { "nixja_output", output_name },
            { "nixja_outputs", edge_outputs },
        });
        for (const Node* input : edge->inputs_) {
            const fs::path path = input->path();
            const Edge* producer = input->in_edge();
            if (path.is_absolute() || !producer || producer->is_phony())
                continue;
            const fs::path destination = (build_relative / path).lexically_normal();
            const std::string reference = edge_map[producer->id_].output_path + "/" + path.generic_string();
            linkPhase += fmt::format(
                // create all the directories up to destination path
                "mkdir -p \"$NIX_BUILD_TOP/{}\"; "
                // -sfn ensures that ln doesn't fail if the destination already exists
                "ln -sfn \"{}\" \"$NIX_BUILD_TOP/{}\"; ",
                destination.parent_path().string(),
                reference,
                destination.string());
        }
    }

    // Add the database to the nix store
    const std::string data = database.dump();
    nix::StringSource source(data);
    const auto db = store->addToStoreFromDump(source, "nixja-compile-commands.json", nix::FileSerialisationMethod::Flat, nix::ContentAddressMethod::Raw::Flat);
    const std::string db_path = store->printStorePath(db);

    spdlog::debug("Creating compilation database at {}", db_path);

    /// Create the derivation that will run nixja-scan-deps, and generate a bunch of mini derivations.
    /// The output of this is a derivation with multiple output possibilities. By output possibility I mean
    /// "out".
    const DerivationSpec spec {
        .name = "compile-scan-batch",
        .storeNamePrefix = "nixja-gen",
        .linkPhase = linkPhase,
        .buildPhase = fmt::format("{} '{}'", NIXJA_SCAN_DEPS_PATH, db_path),
        .outputMethod = "text",
        .outputs = outputs,
        .inputSrcs = json::array({ fs::path(db_path).filename().string() }),
    };
    TRY(create_derivation(edges.front(), spec, true, source_root, source_store_path, build_relative, store, edge_map, edges));
    return { };
}

/// Kahns Algorithm
/// See LC Course Scheduling
/// TODO: only process target closure
Result<void> topo_process(
    const std::string& target,
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

    const Node* target_node = state.LookupNode(target);
    if (target_node == nullptr)
        return Err("Unknown Ninja target '{}'", target);

    // Obtains the entire closure for target node
    InputsCollector collector;
    collector.VisitNode(target_node);

    /// Every edge that produces a node needed by target_node either directly or indirectly
    std::unordered_set<const Edge*> closure;

    // Include the target's own producing edge.
    if (const auto edge = target_node->in_edge())
        closure.insert(edge);

    // Add all edges that produce a dependency (direct or indirect)
    for (const auto node : collector.inputs())
        if (const Edge* edge = node->in_edge())
            closure.insert(edge);

    // Form the adjaceny list
    for (const auto edge : closure) {
        std::unordered_set<size_t> producers;

        InputsCollector edge_inputs;
        edge_inputs.VisitNode(edge->outputs_.front());
        for (const auto node : edge_inputs.inputs()) {
            /// producer -> consumer
            /// producer outputs a node that consumer needs
            const auto producer = node->in_edge();

            // A nullptr means the node doesn't have a dependency
            if (producer && producer != edge && closure.contains(producer))
                producers.insert(producer->id_);
        }

        indegree[edge->id_] = producers.size();

        for (const auto producerId : producers)
            consumers[producerId].push_back(edge);
    }

    for (const Edge* edge : closure)
        if (indegree[edge->id_] == 0)
            queue.push(edge);

    /// TODO: Move this out of this function

    const auto stored = store->addToStore(
        nixja_store_name(source_root.filename().string(), "nixja-gen"),
        nix::makeFSSourceAccessor(source_root),
        nix::ContentAddressMethod::Raw::NixArchive,
        nix::HashAlgorithm::SHA256);

    const std::string source_store_path = store->printStorePath(stored);

    const auto mark_processed = [&](const Edge* edge) {
        for (const auto neighbor : consumers[edge->id_]) {
            if (--indegree[neighbor->id_] == 0)
                queue.push(neighbor);
        }
    };

    //           S1
    //          /  \
    //         v    v
    //        S2    D1 ──┐
    //         |     |   │  Batch 1
    //         v     |   │
    //        D2 ────|───┘
    //         \    /
    //          v  v
    //           D3 ─────── Batch 2
    //           |
    //           v
    //         Done
    //
    // S = Static edge
    // D = Dynamic edge

    clang::DiagnosticOptions diagnostic_options;
    auto diagnostics = clang::CompilerInstance::createDiagnostics(*llvm::vfs::getRealFileSystem(), diagnostic_options);
    clang::driver::Driver compiler("clang++", llvm::sys::getDefaultTargetTriple(), *diagnostics);
    size_t processed = 0;
    while (!queue.empty()) {
        std::vector<const Edge*> dynamic_edges;

        // Process static edges until no more can be processed.
        // Defer dynamic edges for batch processing.
        while (!queue.empty()) {
            const auto edge = queue.front();
            queue.pop();

            llvm::BumpPtrAllocator allocator;
            llvm::StringSaver saver(allocator);
            llvm::SmallVector<const char*> args;
            llvm::cl::TokenizeGNUCommandLine(edge->EvaluateCommand(), saver, args);
            diagnostics->Reset();
            const bool dynamic = is_compile_command(compiler, args);

            {
                std::vector<std::string_view> outputs;
                outputs.reserve(edge->outputs_.size());
                for (const Node* output : edge->outputs_)
                    outputs.emplace_back(output->path());

                spdlog::debug(
                    "{} edge {}: {}",
                    dynamic ? "Deferring dynamic" : "Processing static",
                    edge->id_,
                    fmt::join(outputs, ", "));
            }

            // Defer processing of dynamic edges
            if (dynamic) {
                dynamic_edges.push_back(edge);
                continue;
            }

            TRY(process_static_edge(
                edge,
                source_root,
                source_store_path,
                build_relative,
                store,
                edge_map));

            ++processed;
            mark_processed(edge);
        }

        if (dynamic_edges.empty())
            break;

        // Process every currently available dynamic edge as a batch.
        TRY(process_dynamic_edges(
            dynamic_edges,
            source_root,
            source_store_path,
            build_relative,
            store,
            edge_map));

        processed += dynamic_edges.size();

        // Release consumers only after the entire batch is processed.
        for (const auto edge : dynamic_edges)
            mark_processed(edge);
    }

    if (processed != closure.size())
        return Err("Could not process all Ninja edges for '{}': dependency cycle or unsupported graph", target);

    return { };
}

Result<std::string> nixja_gen(const std::string& target, const fs::path& source_root, const fs::path& build_dir) {

    spdlog::info("[nixja-gen] Generating derivation for target '{}'", target);
    // Load Ninja build graph.
    const fs::path ninja_file = build_dir / "build.ninja";

    // Change CWD to `build_dir` because if the `ninja_file` has any relative includes
    // then it will look relative to the `build_dir`
    fs::current_path(build_dir);

    State state;
    RealDiskInterface disk;
    ManifestParser parser(&state, &disk);

    std::string error;

    if (!parser.Load(ninja_file.string(), &error))
        return Err(error);

    spdlog::debug("Loaded Ninja manifest with {} edges", state.edges_.size());

    fs::current_path(source_root);

    nix::experimentalFeatureSettings.experimentalFeatures.set(
        "ca-derivations dynamic-derivations");
    nix::initLibStore(true);
    const nix::ref<nix::Store> store = nix::openStore();

    std::vector<Derivation> edge_map(state.edges_.size());
    TRY(topo_process(
        target,
        state,
        store,
        edge_map,
        source_root,
        build_dir.lexically_relative(source_root)));

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
        store->parseStorePath(target_derivation.derivation_store_path), target_derivation.output_name);
    const auto generated_output = nix::DownstreamPlaceholder::unknownDerivation(
        generated_drv, "out");

    // `target_derivation.derivation_store_path` is a *.drv.drv.
    // We need to unwrap it to obtain the .drv which we then return
    // and pass to nix-build to turn that .drv into a output

    /// This derivation depends on a *.drv.drv file
    const json finalInputDrvs = {
        { target_derivation.derivation_path, {
                                                 { "dynamicOutputs", {
                                                                         { target_derivation.output_name, {
                                                                                                              { "dynamicOutputs", json::object() },
                                                                                                              { "outputs", json::array({ "out" }) },
                                                                                                          } },
                                                                     } },
                                                 { "outputs", json::array({ target_derivation.output_name }) },
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

    spdlog::info("Generated derivation: {}", store->printStorePath(final_drv));

    return store->printStorePath(final_drv);
}
