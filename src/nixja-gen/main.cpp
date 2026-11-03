#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <inja/inja.hpp>
#include <nlohmann/json.hpp>

#include "disk_interface.h"
#include "graph.h"
#include "manifest_parser.h"
#include "state.h"

#include "nixja-gen.hpp"

namespace fs = std::filesystem;

using json = nlohmann::json;

/// TODO: This is not a good way of doing this
std::string command_for(
    const Edge* edge,
    const std::unordered_set<std::string>& generated_outputs) {
    std::string command = edge->EvaluateCommand();

    for (const Node* input : edge->inputs_) {
        const std::string path = input->path();

        if (!generated_outputs.count(path))
            continue;

        const Edge* producer = input->in_edge();

        if (!producer || producer->outputs_.empty())
            continue;

        // Use the producer edge's first output as its Nix attribute name.
        std::string attr = producer->outputs_[0]->path();

        std::replace(attr.begin(), attr.end(), '/', '_');
        std::replace(attr.begin(), attr.end(), '.', '_');

        const std::string nix_reference = "${" + attr + "}/" + path;

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

    for (const Edge* edge : state.edges_)
        for (const Node* output : edge->outputs_)
            generated_outputs.insert(output->path());

    // Process each edge once.
    for (const Edge* edge : state.edges_) {
        if (edge->outputs_.empty())
            continue;

        std::vector<std::string> outputs;

        for (const auto& output : edge->outputs_) {
            outputs.push_back(output->path());
        }

        const std::string command = command_for(edge, generated_outputs);

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

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
