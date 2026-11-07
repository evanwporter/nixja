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

    std::string derivation_path;

    /// If `generates_drv` is true then this is not empty
    /// The output of this creats the output path.
    /// This comes from derivation_path
    std::string generated_path;

    /// If `generates_drv` is true then this comes from the output of
    /// generated_path. Otherwise this comes from the output of derivation_path.
    std::string output_path;
};

// TODO: I don't think this is used here
//
/// Execute shell command and capture stdout
// Source - https://stackoverflow.com/a/478960
// Posted by waqas, modified by community. See post 'Timeline' for change history
// Retrieved 2026-10-31, License - CC BY-SA 4.0
std::string exec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

std::string command_for(
    const Edge* edge,
    const std::unordered_map<const Node*, Derivation>& node_map) {
    std::string command = edge->EvaluateCommand();

    for (const Node* input : edge->inputs_) {
        const std::string path = input->path();

        const auto node = node_map.find(input);
        if (node == node_map.end())
            continue;

        const std::string& nix_reference = node->second.output_path;

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

/// DepItem
json make_dep_item(
    const std::vector<std::string>& outputs,
    const std::string& command,
    const std::vector<std::string>& dependencies,
    const std::vector<std::string>& implicit_dependencies,
    const std::unordered_set<std::string>& generated_outputs) {
    json deps = json::array();
    json implicit_deps = json::array();

    for (const auto& dependency : dependencies) {
        deps.push_back(
            dependency_for(dependency, generated_outputs));
    }

    for (const auto& dependency : implicit_dependencies) {
        implicit_deps.push_back(
            dependency_for(dependency, generated_outputs));
    }

    // TODO: Potentially combine with above (the next two lines)
    std::string attr = outputs.front();
    std::replace(attr.begin(), attr.end(), '.', '_');

    return {
        { "outputs", outputs },
        { "attrKey", attr },
        { "command", command },
        { "dependencies", deps },
        { "implicit_dependencies", implicit_deps }
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
    std::unordered_map<const Node*, Derivation> node_map;

    for (const Edge* edge : state.edges_) {
        if (edge->outputs_.empty())
            continue;

        std::string attr = edge->outputs_.front()->path();
        std::replace(attr.begin(), attr.end(), '/', '_');
        std::replace(attr.begin(), attr.end(), '.', '_');

        for (const Node* output : edge->outputs_) {
            generated_outputs.insert(output->path());
            node_map[output] = Derivation {
                .output_path = "${" + attr + "}/" + output->path()
            };
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

        const std::string command = command_for(edge, node_map);

        if (has_depfile(command)) {
            std::vector<std::string> dependencies;

            // Identify implicit Ninja inputs.
            std::vector<std::string> implicit_dependencies;

            const size_t implicit_count = edge->implicit_deps_;
            const size_t order_only_count = edge->order_only_deps_;

            const size_t explicit_count = edge->inputs_.size()
                - implicit_count
                - order_only_count;

            for (
                size_t i = explicit_count;
                i < explicit_count + implicit_count;
                ++i) {
                implicit_dependencies.push_back(
                    edge->inputs_[i]->path());
            }

            dep_items.push_back(
                make_dep_item(
                    outputs,
                    command,
                    dependencies,
                    implicit_dependencies,
                    generated_outputs));

        } else {
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

Result<void> process_dep_edge(
    const Edge* edge,
    const std::string& source_store_path,
    const nix::ref<nix::Store> store,
    std::unordered_map<const Node*, Derivation>& node_map) {
    // processing a edge means creating a drv file for it

    std::string linkPhase = "mkdir -p \"$NIX_BUILD_TOP\"; ";
    linkPhase += fmt::format(
        "ln -s {}/src \"$NIX_BUILD_TOP/src\"; ",
        source_store_path);

    // for (const auto [dep, store_path] : store_paths) {
    //     const fs::path parent = fs::path(dep).parent_path();
    //     if (!parent.empty())
    //         linkPhase += fmt::format(
    //             "mkdir -p \"$NIX_BUILD_TOP/{}\"; ", parent.generic_string());
    //     linkPhase += fmt::format("ln -s {} \"$NIX_BUILD_TOP/{}\"; ", store_path, dep);
    // };

    const auto outputs = edge->outputs_;

    const auto nodes = edge->outputs_;

    /// Name of the Output
    auto name = fs::path(nodes.front()->path()).filename().string();
    std::replace(name.begin(), name.end(), '.', '_');

    const std::string command = command_for(edge, node_map);

    /// String passed to `nixja-scan-deps -o`
    std::string outputs_str;
    for (const auto& output : edge->outputs_)
        outputs_str += output->path();

    const std::string nixja_scan_deps = fmt::format("./nixja-scan-deps -i TODO -o {} -- {}", outputs_str, command);

    std::string installPhase = "mkdir -p $out; ";
    for (const auto output : outputs) {
        installPhase += fmt::format(
            "cp {} $out/{}; ", output->path(), fs::path(output->path()).filename().string());
    }

    const char* bashBuilder = std::getenv("NIXJA_BASH");
    if (bashBuilder == nullptr) {
        std::cerr << "NIXJA_BASH is not set; cannot determine derivation builder\n";
        bashBuilder = "/nix/store/kwyi4bq5s2qgwbhz6cs4gjy57c2zdxws-bash-interactive-5.3p20/bin/bash";
        // return std::unexpected("");
    }

    const char* buildPath = std::getenv("NIXJA_PATH");
    if (buildPath == nullptr) {
        std::cerr << "NIXJA_PATH is not set; cannot determine build PATH\n";
        buildPath = "/nix/store/cb987zkxvsv0490753ilrvn5yk7ylgxi-coreutils-9.12/bin:/nix/store/r5ri78axx1qggkjq3vawywlzglg70b5w-gcc-wrapper-16.2.0/bin";
        // return std::unexpected("");
    }

    std::string buildPhase = std::format(R"NIXJA(export NIXJA_BASH={}; export NIXJA_PATH={}; NIXJA_SCAN_DEPS="$({})"; echo $NIXJA_SCAN_DEPS; )NIXJA", bashBuilder, buildPath, nixja_scan_deps);

    std::vector<std::string> inputSrcs;
    inputSrcs.push_back(
        fs::path(source_store_path).filename().string());

    // inputSrcs.reserve(store_paths.size() + 1);
    // for (const auto& [dependency, storePath] : store_paths)
    //     inputSrcs.push_back(fs::path(storePath).filename().string());
    // inputSrcs.push_back(
    //     fs::path(bashBuilder).parent_path().parent_path().filename().string());

    json inputDrvs = {
        { "30hr2syjmfvflwi68acggp3w870ikw5c-bash-5.3p20.drv",
          {
              { "dynamicOutputs", json::object() },
              { "outputs", json::array({ "out" }) },
          } },
        { "hjzzl7y4qy90800c8zk92z9w07l37x2a-gcc-wrapper-16.2.0.drv",
          {
              { "dynamicOutputs", json::object() },
              { "outputs", json::array({ "out" }) },
          } },
    };

    for (const auto node : edge->inputs_) {
        if (node->in_edge()) {
            // This node is the output of another edge
            // So we need the derivation of said node
            if (!node_map.count(node)) {
                std::cerr << "This node is false for some reason\n";
                // return std::unexpected("");
                continue;
            }

            const json dynOutputs = node_map[node].generates_drv
                ? json({ "out", {
                                    { "dynamicOutputs", json::object() },
                                    { "outputs", json::array({ "out" }) },
                                } })
                : json::object();

            inputDrvs.push_back(
                { node_map[node].derivation_path, {
                                                      { "dynamicOutputs", dynOutputs },
                                                      { "outputs", json::array({ "out" }) },
                                                  } });

        } else {
            // // This node is an already existing source path
            // // so we add it to inputSrcs
            // inputSrcs.push_back(node->path());
            // volatile bool node_is_mapped = node_map.count(node) != 0;
        }
    }

    const json inputsJson = {
        { "srcs", inputSrcs },
        // TODO: Replace with values from the toolchain file.
        { "drvs", inputDrvs },
    };

    const json derivationEnv = {
        { "name", "src_main_o" },
        { "PATH", buildPath },
        { "linkPhase", linkPhase },
        // BUG: Ensure every parent directory of an output is created in buildPhase.
        { "buildPhase", buildPhase },
        { "installPhase", installPhase },
        { "outputs", "out" },
        { "out", nix::hashPlaceholder("out") },
    };

    const json outputsJson = {
        { "out", {
                     { "method", "text" },
                     { "hashAlgo", "sha256" },
                 } },
    };

    const json drv = {
        { "name", name },
        { "system", "x86_64-linux" },
        { "builder", bashBuilder },
        { "version", 4u },
        { "args", json::array({ "-e", "-c", "eval \"$linkPhase\"; eval \"$buildPhase\"; eval \"$installPhase\"" }) },
        { "env", derivationEnv },
        { "inputs", inputsJson },
        { "outputs", outputsJson },
    };

    std::cerr << drv << std::endl;

    const auto derivation = nix::Derivation::parseJsonAndValidate(*store, drv);
    const auto drv_path = store->writeDerivation(derivation);
    const auto generated_drv_path = nix::DownstreamPlaceholder::unknownCaOutput(drv_path, "out");
    const auto output_path = nix::DownstreamPlaceholder::unknownDerivation(generated_drv_path, "out").render();

    for (const auto node : edge->outputs_) {
        node_map[node] = Derivation {
            .generates_drv = true,
            // TODO: see if we can get around making a copy here
            .derivation_path = std::string(drv_path.to_string()),
            .generated_path = generated_drv_path.render(),
            .output_path = output_path + "/" + node->path(),
        };
    }

    return { };
};

/// Process a single edge
/// `node_map` is the map of each Node to the Node's corresponding output derivation
///     Multiple nodes can have the same output derivation
// Result<void> process_standard_edge(
//     const Edge* edge,
//     const std::string source_store_dir,
//     std::unordered_map<const Node*, Derivation>& node_map) {
//     // processing a edge means creating a drv file for it
//
//     std::vector<std::string> inputSrcs;
//     // inputSrcs.reserve(store_paths.size() + 1);
//     // for (const auto& [dependency, storePath] : store_paths)
//     //     inputSrcs.push_back(fs::path(storePath).filename().string());
//     // inputSrcs.push_back(
//     //     fs::path(bashBuilder).parent_path().parent_path().filename().string());
//
//     json inputDrvs = {
//         { "30hr2syjmfvflwi68acggp3w870ikw5c-bash-5.3p20.drv",
//           {
//               { "dynamicOutputs", json::object() },
//               { "outputs", json::array({ "out" }) },
//           } },
//         { "hjzzl7y4qy90800c8zk92z9w07l37x2a-gcc-wrapper-16.2.0.drv",
//           {
//               { "dynamicOutputs", json::object() },
//               { "outputs", json::array({ "out" }) },
//           } },
//     };
//
//     for (const auto node : edge->inputs_) {
//         if (node->in_edge()) {
//             // This node is the output of another edge
//             if (!node_map.count(node)) {
//                 std::cerr << "This node is false for some reason\n";
//                 // return std::unexpected("");
//                 continue;
//             }
//
//             inputDrvs.push_back({ node_map[node], {
//                                                       { "dynamicOutputs", json::object() },
//                                                       { "outputs", json::array({ "out" }) },
//                                                   } });
//
//         } else {
//             // This node is an already existing source path
//             // so we add it to inputSrcs
//             inputSrcs.push_back(node->path());
//             volatile bool node_is_mapped = node_map.count(node) != 0;
//         }
//     }
//
//     std::string linkPhase = "mkdir -p \"$NIX_BUILD_TOP\"; ";
//     // for (const auto [dep, store_path] : store_paths) {
//     //     const fs::path parent = fs::path(dep).parent_path();
//     //     if (!parent.empty())
//     //         linkPhase += fmt::format(
//     //             "mkdir -p \"$NIX_BUILD_TOP/{}\"; ", parent.generic_string());
//     //     linkPhase += fmt::format("ln -s {} \"$NIX_BUILD_TOP/{}\"; ", store_path, dep);
//     // };
//
//     const auto outputs = edge->outputs_;
//
//     const auto nodes = edge->outputs_;
//
//     /// Name of the Output
//     auto name = fs::path(nodes.front()->path()).filename().string();
//     std::replace(name.begin(), name.end(), '.', '_');
//
//     const std::string command = command_for(edge, node_map);
//
//     std::string installPhase = "mkdir -p $out; ";
//     for (const auto output : outputs) {
//         installPhase += fmt::format(
//             "cp {} $out/{}; ", output->path(), fs::path(output->path()).filename().string());
//     }
//
//     const char* bashBuilder = std::getenv("NIXJA_BASH");
//     if (bashBuilder == nullptr) {
//         std::cerr << "NIXJA_BASH is not set; cannot determine derivation builder\n";
//         bashBuilder = "";
//         // return std::unexpected("");
//     }
//
//     const char* buildPath = std::getenv("NIXJA_PATH");
//     if (buildPath == nullptr) {
//         std::cerr << "NIXJA_PATH is not set; cannot determine build PATH\n";
//         buildPath = "";
//         // return std::unexpected("");
//     }
//
//     const json inputsJson = {
//         { "srcs", inputSrcs },
//         // TODO: Replace with values from the toolchain file.
//         { "drvs", inputDrvs },
//     };
//
//     const json derivationEnv = {
//         { "name", "src_main_o" },
//         { "PATH", buildPath },
//         { "linkPhase", linkPhase },
//         // BUG: Ensure every parent directory of an output is created in buildPhase.
//         { "buildPhase", command },
//         { "installPhase", installPhase },
//         { "outputs", "out" },
//         { "out", nix::hashPlaceholder("out") },
//     };
//
//     const json outputsJson = {
//         { "out", {
//                      { "method", "nar" },
//                      { "hashAlgo", "sha256" },
//                  } },
//     };
//
//     const json drv = {
//         { "name", name },
//         { "system", "x86_64-linux" },
//         { "builder", bashBuilder },
//         { "version", 4u },
//         { "args", json::array({ "-e", "-c", "eval \"$linkPhase\"; eval \"$buildPhase\"; eval \"$installPhase\"" }) },
//         { "env", derivationEnv },
//         { "inputs", inputsJson },
//         { "outputs", outputsJson },
//     };
//
//     std::cerr << drv << std::endl;
//
//     for (const auto node : edge->outputs_)
//         node_map[node] = name;
//
//     return { };
// };

/// Kahns Algorithm
/// See LC Course Scheduling
void topo_process(const State& state, const nix::ref<nix::Store> store) {
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

    std::unordered_map<const Node*, Derivation> node_map;

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
        return rel == "." || rel == "src" || rel.starts_with("src/");
    };
    const auto stored = store->addToStore(
        "source",
        nix::PosixSourceAccessor::createAtRoot(project_root),
        nix::ContentAddressMethod::Raw::NixArchive,
        nix::HashAlgorithm::SHA256,
        { },
        only_src);

    const std::string source_store_path = store->printStorePath(stored);

    std::cerr << "SOURCE STORE PATH " << source_store_path << std::endl;

    while (!queue.empty()) {
        const auto edge = queue.front();
        queue.pop();

        if (!has_depfile(edge->EvaluateCommand())) {
            // const auto result = process_standard_edge(edge, source_store_path, node_map);
            // if (!result.has_value())
            //     // TODO: Don't return but handle it better
            //     return;

        } else {
            const auto result = process_dep_edge(edge, source_store_path, store, node_map);
            if (!result.has_value())
                return;
        }

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

        // Construct Inja data.
        json data = {
            { "standardItems", standard_items },
            { "depItems", dep_items },
            { "target", target }
        };

        inja::Environment env;

        std::cout << env.render(nixja_gen_template, data);

        nix::experimentalFeatureSettings.experimentalFeatures.set(
            "ca-derivations dynamic-derivations");
        nix::initLibStore(true);
        const nix::ref<nix::Store> store = nix::openStore();

        topo_process(state, store);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
