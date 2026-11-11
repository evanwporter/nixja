#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include <fmt/format.h>

#include <nix/main/shared.hh>
#include <nix/store/derivations.hh>
#include <nix/store/downstream-placeholder.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-dir-config.hh>
#include <nix/store/store-open.hh>
#include <nix/util/posix-source-accessor.hh>

#include "common.hpp"
#include "toolchain.hpp"

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
    std::string generated_path;

    /// If `generates_drv` is true then this comes from the output of
    /// generated_path. Otherwise this comes from the output of derivation_path.
    std::string output_path;
};

/// Returns the command with the inputs appropriately substituted
/// TODO: This is a bad way of doing it I think. Better to hack onto EvaluateCommand somehow
std::string command_for(
    const Edge* edge,
    const std::vector<Derivation>& edge_map) {
    std::string command = edge->EvaluateCommand();

    for (const Node* input : edge->inputs_) {
        const std::string path = input->path();

        const Edge* producer = input->in_edge();
        if (producer == nullptr)
            continue;

        const std::string nix_reference = edge_map[producer->id_].output_path + "/" + path;

        size_t pos = 0;
        while ((pos = command.find(path, pos)) != std::string::npos) {
            command.replace(pos, path.size(), nix_reference);
            pos += nix_reference.size();
        }
    }

    return command;
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

// TODO: Combine the next two functions into one simpler one
// The only difference is how it forms the dep list

Result<void> create_derivation(
    const Edge* edge,
    const DerivationSpec& spec,
    const bool generates_drv,
    const std::string& source_store_path,
    const nix::ref<nix::Store> store,
    std::vector<Derivation>& edge_map) {
    // processing a edge means creating a drv file for it

    /// TODO: `cp -rs` symlinks every file. We only want to symlink top level files and folders
    const std::string linkPhase = fmt::format(
        "mkdir -p \"$NIX_BUILD_TOP\"; cp -rs {}/. \"$NIX_BUILD_TOP\"; ",
        source_store_path);

    // Toolchain configuration.
    const char* bashBuilder = toolchain::bash;

    const char* buildPath = toolchain::build_path;

    // Collect source inputs.
    std::vector<std::string> inputSrcs;

    inputSrcs.push_back(
        fs::path(source_store_path).filename().string());

    inputSrcs.push_back(toolchain::clang_tools_store_name);
    inputSrcs.push_back(toolchain::bash_store_name);
    inputSrcs.push_back(toolchain::coreutils_store_name);
    inputSrcs.push_back(toolchain::cc_store_name);
    if (toolchain::nixja_scan_deps_store_name[0] != '\0')
        inputSrcs.push_back(toolchain::nixja_scan_deps_store_name);
    inputSrcs.push_back(
        fs::path(toolchain::nix_store_package_path).filename().string());
    inputSrcs.push_back(
        fs::path(toolchain::nix_util_package_path).filename().string());
    inputSrcs.push_back(
        fs::path(toolchain::fmt_package_path).filename().string());

    // Collect input derivations.
    json inputDrvs = json::object();

    for (const auto node : edge->inputs_) {
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
    completeSpec.system = "x86_64-linux";
    completeSpec.builder = bashBuilder;
    completeSpec.path = buildPath;
    completeSpec.linkPhase = linkPhase;
    completeSpec.inputSrcs = inputSrcs;
    completeSpec.inputDrvs = inputDrvs;
    completeSpec.extraEnv = {
        { "NIXJA_PATH", buildPath },
        { "NIXJA_BASH", bashBuilder },
    };
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
        .generated_path = generated_path,
        .output_path = output_path,
    };

    return { };
};

Result<void> process_standard_edge(
    const Edge* edge,
    const std::string& source_store_path,
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
    const std::string command = command_for(edge, edge_map);

    const std::string buildPhase = fmt::format("{0}; echo {0};", command);

    // Construct install phase.
    std::string installPhase = "mkdir -p $out;";

    for (const auto& output : edge->outputs_) {
        installPhase += fmt::format(
            "cp {0} $out/{0}; ",
            output->path());
    }

    // Configure derivation.
    const DerivationSpec spec {
        .name = derivationName,
        .buildPhase = buildPhase,
        .installPhase = installPhase,
        .outputMethod = "nar",
    };

    TRY(create_derivation(edge, spec, false, source_store_path, store, edge_map));

    return { };
}

Result<void> process_dep_edge(
    const Edge* edge,
    const std::string& source_store_path,
    const nix::ref<nix::Store> store,
    std::vector<Derivation>& edge_map) {

    // Determine derivation name.
    const auto name = fs::path(edge->outputs_.front()->path()).filename().string();

    std::string derivationName = name;
    std::replace(derivationName.begin(), derivationName.end(), '.', '_');

    // A derivation-producing output must end in .drv.
    derivationName += ".drv";

    // Construct command.
    const std::string command = command_for(edge, edge_map);

    // Construct scanner output arguments.
    std::string outputs_str;

    for (const auto& output : edge->outputs_)
        outputs_str += output->path();

    const std::string nixja_scan_deps = fmt::format(
        "{} -i TODO -o {} -- {}",
        toolchain::nixja_scan_deps,
        outputs_str,
        command);

    const std::string installPhase = "cp \"$NIXJA_SCAN_DEPS\" \"$out\"; ";

    const std::string buildPhase = std::format(
        R"NIXJA(NIXJA_SCAN_DEPS="$({})"; echo $NIXJA_SCAN_DEPS; )NIXJA",
        nixja_scan_deps);

    // Configure derivation.
    const DerivationSpec spec {
        .name = derivationName,
        .buildPhase = buildPhase,
        .installPhase = installPhase,
        .outputMethod = "text",
    };

    TRY(create_derivation(edge, spec, true, source_store_path, store, edge_map));

    return { };
}

/// Kahns Algorithm
/// See LC Course Scheduling
Result<void> topo_process(
    const State& state,
    const nix::ref<nix::Store> store,
    std::vector<Derivation>& edge_map) {
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

    const fs::path source_dir = fs::canonical(fs::current_path());

    const auto stored = store->addToStore(
        source_dir.filename().string(), // "nixja", not "."
        nix::PosixSourceAccessor::createAtRoot(source_dir),
        nix::ContentAddressMethod::Raw::NixArchive);

    const std::string source_store_path = store->printStorePath(stored);

    // std::cerr << "SOURCE STORE PATH " << source_store_path << std::endl;

    while (!queue.empty()) {
        const auto edge = queue.front();
        queue.pop();

        if (has_depfile(edge->EvaluateCommand()))
            TRY(process_dep_edge(edge, source_store_path, store, edge_map));
        else
            TRY(process_standard_edge(edge, source_store_path, store, edge_map));

        for (const auto neighbor : consumers[edge->id_]) {
            indegree[neighbor->id_]--;

            if (indegree[neighbor->id_] == 0)
                queue.push(neighbor);
        }
    }

    return { };
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: nixja <target>\n";
        return 1;
    }

    std::string target = argv[1];

    // Load Ninja build graph.
    fs::path ninja_file = fs::current_path() / "build.ninja";

    State state;
    RealDiskInterface disk;
    ManifestParser parser(&state, &disk);

    std::string error;

    if (!parser.Load(ninja_file.string(), &error)) {
        std::cerr << "Parse error: " << error << '\n';
        return 1;
    }

    nix::experimentalFeatureSettings.experimentalFeatures.set(
        "ca-derivations dynamic-derivations");
    nix::initLibStore(true);
    const nix::ref<nix::Store> store = nix::openStore();

    std::vector<Derivation> edge_map(state.edges_.size());
    MAIN_TRY(topo_process(state, store, edge_map));

    const Node* node = state.LookupNode(target);

    if (node == nullptr) {
        // No such target in the loaded build graph.
        // TODO: error
        return 1;
    }

    const Edge* target_edge = node->in_edge();

    if (target_edge == nullptr) {
        // The target is a source file or has no producing build edge.
        // TODO: Error
        return 1;
    }

    std::cout << edge_map[target_edge->id_].derivation_store_path << '\n';

    return 0;
}
