#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fmt/format.h>
#include <iostream>
#include <nix/store/downstream-placeholder.hh>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <inja/inja.hpp>
#include <nlohmann/json.hpp>

#include <nix/main/shared.hh>
#include <nix/store/derivations.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-dir-config.hh>
#include <nix/store/store-open.hh>
#include <nix/util/posix-source-accessor.hh>

#include "disk_interface.h"
#include "graph.h"
#include "manifest_parser.h"
#include "state.h"

#include "nixja-gen.hpp"

namespace fs = std::filesystem;

using json = nlohmann::json;

template <typename T, typename E = std::string>
using Result = std::expected<T, E>;

// TODO: Clarify how each field is used
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

std::string command_for(
    const Edge* edge,
    const std::unordered_map<const Edge*, Derivation>& edge_map) {
    std::string command = edge->EvaluateCommand();

    for (const Node* input : edge->inputs_) {
        const std::string path = input->path();

        const Edge* producer = input->in_edge();
        if (producer == nullptr)
            continue;

        const auto derivation = edge_map.find(producer);
        if (derivation == edge_map.end())
            continue;

        const std::string nix_reference = derivation->second.output_path + "/" + path;

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

/// Dependency representation
json dependency_for(
    const std::string& value,
    const std::unordered_set<std::string>& generated_outputs) {
    bool is_node = generated_outputs.find(value)
        != generated_outputs.end();

    std::string attr = value;

    std::replace(attr.begin(), attr.end(), '.', '_');

    return {
        { "value", value },
        { "is_node", is_node },
        { "rendered", is_node ? "${" + attr + "}" : value },
        { "source_path", "./" + value }
    };
}

// TODO: Combine the next two functions into one simpler one
// The only difference is how it forms the dep list

/// StandardItem
json make_standard_item(
    const std::vector<std::string>& outputs,
    const std::string& command,
    const std::vector<std::string>& dependencies,
    const std::unordered_set<std::string>& generated_outputs) {
    json deps = json::array();

    for (const auto& dependency : dependencies) {
        deps.push_back(
            dependency_for(dependency, generated_outputs));
    }

    // TODO: Check if outputs is empty?

    std::string attr = outputs.front();

    std::replace(attr.begin(), attr.end(), '.', '_');

    return {
        { "outputs", outputs },
        { "attrKey", attr },
        { "command", command },
        { "dependencies", deps }
    };
}

/// Construct template items from Ninja graph
/// A single item corresponds to a derivation in nix and a build edge in ninja.
void build_items(
    State& state,
    json& standard_items,
    json& dep_items) {
    // Collect every generated output.
    std::unordered_set<std::string> generated_outputs;
    std::unordered_map<const Edge*, Derivation> edge_map;

    for (const Edge* edge : state.edges_) {
        if (edge->outputs_.empty())
            continue;

        std::string attr = edge->outputs_.front()->path();
        std::replace(attr.begin(), attr.end(), '/', '_');
        std::replace(attr.begin(), attr.end(), '.', '_');

        edge_map[edge] = Derivation {
            .output_path = "${" + attr + "}"
        };

        for (const Node* output : edge->outputs_) {
            generated_outputs.insert(output->path());
        }
    }

    // Process each edge once.
    for (const Edge* edge : state.edges_) {
        if (edge->outputs_.empty())
            continue;

        std::vector<std::string> outputs;

        for (const auto& output : edge->outputs_) {
            outputs.push_back(output->path());
        }

        const std::string command = command_for(edge, edge_map);

        if (!has_depfile(command)) {
            std::vector<std::string> dependencies;

            for (const Node* input : edge->inputs_) {
                const std::string path = input->path();

                if (generated_outputs.find(path)
                    == generated_outputs.end()) {
                    dependencies.push_back(path);
                }
            }

            standard_items.push_back(
                make_standard_item(
                    outputs,
                    command,
                    dependencies,
                    generated_outputs));
        }
    }
}

void process_standard_edge(
    const Edge* edge,
    const std::string& command,
    std::string& buildPhase,
    std::string& installPhase) {
    buildPhase = fmt::format("{0}; echo {0};", command);

    installPhase = "mkdir -p $out;";
    for (const auto output : edge->outputs_) {
        // TODO: copy the path all the way down
        installPhase += fmt::format("cp {0} $out/{0}", output->path());
    }
}

Result<void> process_dep_edge(
    const Edge* edge,
    const std::string& source_store_path,
    const nix::ref<nix::Store> store,
    std::unordered_map<const Edge*, Derivation>& edge_map) {
    // processing a edge means creating a drv file for it

    std::string linkPhase = "mkdir -p \"$NIX_BUILD_TOP\"; ";
    linkPhase += fmt::format(
        "ln -s {}/src \"$NIX_BUILD_TOP/src\"; ",
        source_store_path);
    linkPhase += fmt::format(
        "ln -s {}/nixja-scan-deps \"$NIX_BUILD_TOP/nixja-scan-deps\"; ",
        source_store_path);
    linkPhase += fmt::format(
        "ln -s {}/lib*.so* \"$NIX_BUILD_TOP/\"; ",
        source_store_path);

    const auto outputs = edge->outputs_;

    const auto nodes = edge->outputs_;

    /// Name of the Output
    auto name = fs::path(nodes.front()->path()).filename().string();
    std::replace(name.begin(), name.end(), '.', '_');

    // A derivation-producing output must itself end in `.drv`. Nix uses that
    // suffix to parse the first dynamic output as a derivation path.
    const std::string derivationName = name + ".drv";

    const std::string command = command_for(edge, edge_map);

    std::string buildPhase;
    std::string installPhase;
    if (has_depfile(command)) {
        /// String passed to `nixja-scan-deps -o`
        std::string outputs_str;
        for (const auto& output : edge->outputs_)
            outputs_str += output->path();

        const std::string nixja_scan_deps = fmt::format(
            "./nixja-scan-deps -i TODO -o {} -- {}",
            outputs_str,
            command);

        // This is the scanning stage, not the compilation stage. Its output
        // is the derivation emitted by nixja-scan-deps; the derivation itself
        // builds the object files once Nix resolves the dynamic output.
        installPhase = "cp \"$NIXJA_SCAN_DEPS\" \"$out\"; ";
        buildPhase = std::format(
            R"NIXJA(NIXJA_SCAN_DEPS="$({})"; echo $NIXJA_SCAN_DEPS; )NIXJA",
            nixja_scan_deps);
    } else {
        process_standard_edge(edge, command, buildPhase, installPhase);
    }

    const char* bashBuilder = "/nix/store/nnm9gdr70ihc6zc9298mzz8jpcgaciql-bash-5.3p20/bin/bash";
    const char* buildPath = "/nix/store/g2ycks7l2dz7rr4l6cyxf4in7qrfjk8j-clang-tools-21.1.8/bin:/nix/store/cb987zkxvsv0490753ilrvn5yk7ylgxi-coreutils-9.12/bin:/nix/store/r5ri78axx1qggkjq3vawywlzglg70b5w-gcc-wrapper-16.2.0/bin";

    std::vector<std::string> inputSrcs;
    inputSrcs.push_back(
        fs::path(source_store_path).filename().string());
    inputSrcs.push_back(
        "g2ycks7l2dz7rr4l6cyxf4in7qrfjk8j-clang-tools-21.1.8");

    // inputSrcs.reserve(store_paths.size() + 1);
    // for (const auto& [dependency, storePath] : store_paths)
    //     inputSrcs.push_back(fs::path(storePath).filename().string());
    // inputSrcs.push_back(
    //     fs::path(bashBuilder).parent_path().parent_path().filename().string());

    json inputDrvs = json::object();
    for (const char* derivation_path : {
             "30hr2syjmfvflwi68acggp3w870ikw5c-bash-5.3p20.drv",
             "m7bmd2qz1zcnk4k25y5cg6wbb3y9qwcb-coreutils-9.12.drv",
             "hjzzl7y4qy90800c8zk92z9w07l37x2a-gcc-wrapper-16.2.0.drv",
         }) {
        inputDrvs[derivation_path] = {
            { "dynamicOutputs", json::object() },
            { "outputs", json::array({ "out" }) },
        };
    }

    for (const auto node : edge->inputs_) {
        if (const Edge* producer = node->in_edge()) {
            // This node is the output of another edge
            // So we need the derivation of said node
            const auto derivation = edge_map.find(producer);
            if (derivation == edge_map.end()) {
                std::cerr << "This node is false for some reason\n";
                // return std::unexpected("");
                continue;
            }

            const json dynOutputs = derivation->second.generates_drv
                ? json::object({ { "out", {
                                              { "dynamicOutputs", json::object() },
                                              { "outputs", json::array({ "out" }) },
                                          } } })
                : json::object();

            inputDrvs[derivation->second.derivation_path] = {
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

    const json inputsJson = {
        { "srcs", inputSrcs },
        // TODO: Replace with values from the toolchain file.
        { "drvs", inputDrvs },
    };

    const json derivationEnv = {
        { "name", derivationName },
        { "PATH", buildPath },
        { "NIXJA_PATH", buildPath },
        { "NIXJA_BASH", bashBuilder },
        { "linkPhase", linkPhase },
        // BUG: Ensure every parent directory of an output is created in buildPhase.
        { "buildPhase", buildPhase },
        { "installPhase", installPhase },
        { "outputs", "out" },
        { "requiredSystemFeatures", "recursive-nix" },
        { "out", nix::hashPlaceholder("out") },
    };

    const json outputsJson = {
        { "out", {
                     { "method", "text" },
                     { "hashAlgo", "sha256" },
                 } },
    };

    const json drv = {
        { "name", derivationName },
        { "system", "x86_64-linux" },
        { "builder", bashBuilder },
        { "version", 4u },
        { "args", json::array({ "-e", "-c", "eval \"$linkPhase\"; eval \"$buildPhase\"; eval \"$installPhase\"" }) },
        { "env", derivationEnv },
        { "inputs", inputsJson },
        { "outputs", outputsJson },
    };

    const auto derivation = nix::Derivation::parseJsonAndValidate(*store, drv);
    const auto drv_path = store->writeDerivation(derivation);
    const auto generated_drv_path = nix::DownstreamPlaceholder::unknownCaOutput(drv_path, "out");
    const auto output_path = nix::DownstreamPlaceholder::unknownDerivation(generated_drv_path, "out").render();

    edge_map[edge] = Derivation {
        .generates_drv = has_depfile(command),
        // TODO: see if we can get around making a copy here
        .derivation_path = std::string(drv_path.to_string()),
        .derivation_store_path = store->printStorePath(drv_path),
        .generated_path = generated_drv_path.render(),
        .output_path = output_path,
    };

    return { };
};

/// Kahns Algorithm
/// See LC Course Scheduling
void topo_process(
    const State& state,
    const nix::ref<nix::Store> store,
    std::unordered_map<const Edge*, Derivation>& edge_map) {
    // TODO: Use vector instead of unordered_map

    /// The number of unprocessed dependencies for a give edge
    std::unordered_map<const Edge*, size_t> indegree;

    /// Adjaceny list
    /// Maps each edge to all the edges that depend on it (waiting on this edge to be processed)
    std::unordered_map<const Edge*, std::vector<const Edge*>> consumers;

    /// Ready Queue
    /// A edge in this queue means that all of its dependencies have been processed
    std::queue<const Edge*> queue;

    // Form the adjaceny list
    for (const auto edge : state.edges_) {
        std::unordered_set<const Edge*> producers;

        for (const auto node : edge->inputs_) {
            /// producer -> consumer
            /// producer outputs a node that consumer needs
            const auto producer = node->in_edge();

            // A nullptr means the node doesn't have a dependency
            if (producer)
                producers.insert(producer);
        }

        indegree[edge] = producers.size();

        for (const Edge* producer : producers) {
            consumers[producer].push_back(edge);
        }
    }

    for (const auto* edge : state.edges_) {
        if (indegree[edge] == 0) {
            queue.push(edge);
        }
    }

    /// TODO: Move this out of this function

    // const fs::path source_dir = fs::canonical(fs::current_path());
    //
    // const auto stored = store->addToStore(
    //     source_dir.filename().string(), // "nixja", not "."
    //     nix::PosixSourceAccessor::createAtRoot(source_dir),
    //     nix::ContentAddressMethod::Raw::NixArchive);
    //
    // const std::string source_store_path = store->printStorePath(stored);

    // TODO: This filtering mechanism needs to be removed. Its only here so i don't keep
    // adding the slightly changes directories to the nix store
    const fs::path project_root = fs::canonical(fs::current_path());
    nix::PathFilter only_src = [project_root](const std::string& path) {
        const fs::path relative = fs::path(path).lexically_relative(project_root);
        const std::string rel = relative.generic_string();
        return rel == "."
            || rel == "src"
            || rel.starts_with("src/")
            || rel == "nixja-scan-deps"
            || (rel.starts_with("lib") && !rel.contains('/'));
    };
    const auto stored = store->addToStore(
        "source",
        nix::PosixSourceAccessor::createAtRoot(project_root),
        nix::ContentAddressMethod::Raw::NixArchive,
        nix::HashAlgorithm::SHA256,
        { },
        only_src);

    const std::string source_store_path = store->printStorePath(stored);

    // std::cerr << "SOURCE STORE PATH " << source_store_path << std::endl;

    while (!queue.empty()) {
        const auto edge = queue.front();
        queue.pop();

        const auto result = process_dep_edge(edge, source_store_path, store, edge_map);
        if (!result.has_value())
            return;

        for (const auto neighbor : consumers[edge]) {
            indegree[neighbor]--;

            if (indegree[neighbor] == 0)
                queue.push(neighbor);
        }
    }
}

int main(int argc, char* argv[]) {
    try {
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

        // Construct template items.
        json standard_items = json::array();
        json dep_items = json::array();

        build_items(
            state,
            standard_items,
            dep_items);

        nix::experimentalFeatureSettings.experimentalFeatures.set(
            "ca-derivations dynamic-derivations");
        nix::initLibStore(true);
        const nix::ref<nix::Store> store = nix::openStore();

        std::unordered_map<const Edge*, Derivation> edge_map;
        topo_process(state, store, edge_map);

        std::vector<json> dep_items_new;
        for (const auto [k, v] : edge_map) {
            if (v.generates_drv) {
                std::string attr = k->outputs_.front()->path();
                std::replace(attr.begin(), attr.end(), '/', '_');
                std::replace(attr.begin(), attr.end(), '.', '_');
                dep_items_new.push_back({ { attr, v.derivation_store_path } });
            }
        }

        std::vector<json> standard_items_new;
        for (const auto [k, v] : edge_map) {
            if (!v.generates_drv) {
                std::string attr = k->outputs_.front()->path();
                std::replace(attr.begin(), attr.end(), '/', '_');
                std::replace(attr.begin(), attr.end(), '.', '_');
                standard_items_new.push_back({ { attr, v.derivation_store_path } });
            }
        }

        // Construct Inja data.
        json data = {
            { "standardItems", standard_items_new },
            { "depItems", dep_items_new },
            { "target", target }
        };

        inja::Environment env;

        std::cout << env.render(nixja_gen_template, data);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
