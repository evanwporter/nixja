#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include <argparse/argparse.hpp>

#include "logging.hpp"
#include "nixja.hpp"

int main(int argc, char* argv[]) {
    init_logging("nixja", spdlog::level::debug);

    // TODO: To replicate NINJA we should copy the cmd line format from libninja
    argparse::ArgumentParser program("nixja", NIXJA_VERSION);
    program.add_description("A Ninja compatible build system for Nix.");
    program.add_argument("target")
        .help("Build the specified target")
        .metavar("TARGET");
    program.add_argument("-C")
        .help("Change to directory before doing anything else")
        .default_value("./.")
        .metavar("DIR");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    const auto result = nixja(program.get<std::string>("target"), program.get<std::string>("-C"));

    if (!result.has_value()) {
        spdlog::error(result.error());
        return 1;
    }

    return 0;
}
