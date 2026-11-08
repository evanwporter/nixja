#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <inja/inja.hpp>
#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include "nixja.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

// TODO: This can be made simpler
void run_command(const std::vector<std::string>& args) {
    std::vector<char*> argv;

    for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));

    argv.push_back(nullptr);

    pid_t pid = fork();

    if (pid == -1)
        throw std::runtime_error("fork() failed");

    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status;

    if (waitpid(pid, &status, 0) == -1)
        throw std::runtime_error("waitpid() failed");

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("Command failed: " + args[0]);
}

// TODO: allow specifying a store that's not /nix/store
// TODO: Test if I can run this in a standard nix derivation as if it was ninja or something
// TODO: Add the -C arg like ninja -C build
int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: nixja <target>\n";
            return 1;
        }

        std::string target = argv[1];

        fs::path output_path = fs::current_path() / "ninja.nix";

        // Render embedded template.
        inja::Environment env;

        json data = {
            { "target", target }
        };

        // TODO: see if inja supports creating the file directly

        const std::string rendered = env.render(nixja_template, data);

        // Write generated Nix file.
        {
            std::ofstream output(output_path);

            if (!output)
                throw std::runtime_error("Could not open output file");

            output << rendered;
        }

        // Build generated Nix expression.
        // TODO: don't use nix-build (so we don't have to require recursive-nix in parent drv)
        // to solve this I might need to steal stuff from nix-ninja
        // TODO: use NIXPATH to obtain `pkgs`
        run_command({ "nix-build", "--extra-experimental-features", "ca-derivations dynamic-derivations", "--show-trace", "-E", "import " + output_path.string() + " { pkgs = import <nixpkgs> {}; }" });

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
