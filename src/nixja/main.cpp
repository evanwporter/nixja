#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <argparse/argparse.hpp>
#include <fmt/format.h>
#include <nix/store/store-api.hh>
#include <nix/util/source-accessor.hh>
#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include "common.hpp"
#include "nixja_gen.hpp"

using json = nlohmann::json;

// TODO: This can be made simpler
void run_command(const std::vector<std::string>& args) {
    std::vector<char*> argv;

    for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));

    argv.push_back(nullptr);

    pid_t pid = fork();

    if (pid == -1)
        // TODO: error
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
    argparse::ArgumentParser program("nixja", NIXJA_VERSION);
    program.add_description(
        "A Ninja compatible build system for Nix.");
    program.add_argument("target")
        .help("Build the specified target")
        .metavar("TARGET");
    program.add_argument("--store")
        .help("Nix store path")
        .default_value("")
        .metavar("PATH");

    try {
        program.parse_args(argc, argv); // Example: ./main --input_files config.yml System.xml
    } catch (const std::exception& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        std::exit(1);
    }

    const auto target = program.get<std::string>("target");
    const auto drv_result = nixja_gen(target);
    MAIN_TRY(drv_result);
    const auto drv_path = std::move(drv_result.value());

    // Building this one derivation causes Nix to resolve the generator's
    // dynamic output and build the derivation it emits.
    run_command({ "nix", "build", "--quiet", "--extra-experimental-features", "ca-derivations dynamic-derivations", "--show-trace", "--out-link", "result", drv_path + "^out" });

    return 0;
}
