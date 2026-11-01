#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
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

#include "nixja-gen.j2.hpp"

namespace fs = std::filesystem;

using json = nlohmann::json;

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

/// Parse Makefile dependency output from clang-scan-deps
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
    fs::path source_root = fs::canonical(fs::current_path());

    std::unordered_set<std::string> local;

    for (const auto& dependency : dependencies) {
        fs::path path(dependency);

        fs::path resolved = fs::weakly_canonical(
            path.is_absolute() ? path : source_root / path);

        auto relative = resolved.lexically_relative(source_root);

        if (relative.empty() || relative.is_absolute())
            continue;

        if (*relative.begin() == "..")
            continue;

        local.insert(relative.generic_string());
    }

    return local;
}

/// Scan compiler dependencies
std::string shell_quote(const std::string& value) {
    std::string result = "'";

    for (char c : value) {
        if (c == '\'')
            result += "'\\''";
        else
            result += c;
    }

    return result + "'";
}

std::unordered_map<std::string, std::unordered_set<std::string>>
scan_dependencies() {
    const fs::path compdb_file = fs::current_path() / "compile_commands.json";

    if (!fs::is_regular_file(compdb_file))
        throw std::runtime_error(
            "Could not find compilation database: " + compdb_file.string());

    // Run clang-scan-deps.
    const std::string command = "clang-scan-deps -compilation-database=" + shell_quote(compdb_file.string());

    const std::string output = exec(command.c_str());

    auto dependency_map = parse_makefile(output);

    for (auto& [target, dependencies] : dependency_map)
        dependencies = local_dependencies(dependencies);

    return dependency_map;
}

std::string command_for(
    const Edge* edge,
    const std::unordered_set<std::string>& generated_outputs) {
    std::string command = edge->EvaluateCommand();

    for (const Node* input : edge->inputs_) {
        std::string path = input->path();

        if (!generated_outputs.count(path))
            continue;

        std::string attr = path;
        std::replace(attr.begin(), attr.end(), '.', '_');

        // Replace occurrences of the input path with its Nix reference.
        size_t pos = 0;

        while ((pos = command.find(path, pos)) != std::string::npos) {
            command.replace(
                pos,
                path.size(),
                "${" + attr + "}");

            pos += attr.size() + 3;
        }
    }

    return command;
}

/// Determine whether a command uses a depfile
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

/// StandardItem
json make_standard_item(
    const std::string& output,
    const std::string& command,
    const std::vector<std::string>& dependencies,
    const std::unordered_set<std::string>& generated_outputs) {
    json deps = json::array();

    for (const auto& dependency : dependencies) {
        deps.push_back(
            dependency_for(dependency, generated_outputs));
    }

    std::string attr = output;

    std::replace(attr.begin(), attr.end(), '.', '_');

    return {
        { "output", output },
        { "attrKey", attr },
        { "command", command },
        { "dependencies", deps }
    };
}

/// DepItem
json make_dep_item(
    const std::string& output,
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

    std::string attr = output;

    std::replace(attr.begin(), attr.end(), '.', '_');

    return {
        { "output", output },
        { "attrKey", attr },
        { "command", command },
        { "dependencies", deps },
        { "implicit_dependencies", implicit_deps }
    };
}

/// Construct template items from Ninja graph
void build_items(
    State& state,
    const std::unordered_map<
        std::string,
        std::unordered_set<std::string>>& dependency_map,
    json& standard_items,
    json& dep_items) {
    // Collect every generated output.
    std::unordered_set<std::string> generated_outputs;

    for (const Edge* edge : state.edges_) {
        for (const Node* output : edge->outputs_)
            generated_outputs.insert(output->path());
    }

    // Process each edge once.
    for (const Edge* edge : state.edges_) {
        if (edge->outputs_.empty())
            continue;

        std::string output = edge->outputs_[0]->path();

        std::string command = command_for(edge, generated_outputs);

        if (has_depfile(command)) {
            std::vector<std::string> dependencies;

            auto it = dependency_map.find(output);

            if (it != dependency_map.end()) {
                for (const auto& dependency : it->second) {
                    if (generated_outputs.find(dependency)
                        == generated_outputs.end()) {
                        dependencies.push_back(dependency);
                    }
                }
            }

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
                    output,
                    command,
                    dependencies,
                    implicit_dependencies,
                    generated_outputs));

        } else {
            std::vector<std::string> dependencies;

            for (const Node* input : edge->inputs_) {
                std::string path = input->path();

                if (generated_outputs.find(path)
                    == generated_outputs.end()) {
                    dependencies.push_back(path);
                }
            }

            standard_items.push_back(
                make_standard_item(
                    output,
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

        // Scan compiler dependencies.
        auto dependency_map = scan_dependencies();

        // Construct template items.
        json standard_items = json::array();
        json dep_items = json::array();

        build_items(
            state,
            dependency_map,
            standard_items,
            dep_items);

        // Construct Inja data.
        json data = {
            { "standardItems", standard_items },
            { "depItems", dep_items },
            { "target", target }
        };

        // Locate and render template.
        fs::path template_path = fs::path(__FILE__).parent_path() / "nixja-gen.nix.j2";

        inja::Environment env;

        std::cout << env.render(nixja_gen_template, data);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
