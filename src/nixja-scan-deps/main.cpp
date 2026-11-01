#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <inja/environment.hpp>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <inja/inja.hpp>
#include <nlohmann/json.hpp>

#include "nixja-scan-deps.hpp"

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
    all_dependencies = local_dependencies(all_dependencies);

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

    // Render embedded template.
    inja::Environment env;

    /// TODO: Maybe give a error or something here?
    if (outputs.empty())
        return 0;

    std::string name = outputs.front();

    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '.', '_');

    const json data = {
        { "name", name },
        { "outputs", outputs },
        { "command", command },
        { "dependencies", dependencies }
    };

    const std::string rendered = env.render(nixja_scan_deps_template, data);

    std::cout << rendered;
}
