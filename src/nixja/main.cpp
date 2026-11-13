#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <argparse/argparse.hpp>
#include <fmt/format.h>
#include <nix/store/store-api.hh>
#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include "common.hpp"

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

// TODO: Split up into outer and inner derivation generation
// TODO: name == target
// TODO: Like 90% of this is copied from nixja-gen, so we need to create a common function
Result<std::string> build_drv_file(
    const std::string& target,
    const std::string& store_path) {
    nix::experimentalFeatureSettings.experimentalFeatures.set(
        "ca-derivations dynamic-derivations");
    nix::initLibStore(true);
    const nix::ref<nix::Store> store = nix::openStore(store_path);

    const fs::path project_root = fs::canonical(fs::current_path());
    // Keep the complete project tree together.  The builder mounts this one
    // store path at $NIX_BUILD_TOP/source rather than recreating selected
    // files and directories individually.
    nix::PathFilter whole_project = [](const std::string&) {
        return true;
    };

    const auto stored = store->addToStore(
        nixja_store_name("source"),
        nix::PosixSourceAccessor::createAtRoot(project_root),
        nix::ContentAddressMethod::Raw::NixArchive,
        nix::HashAlgorithm::SHA256,
        { },
        whole_project);

    const std::string source_store_path = store->printStorePath(stored);

    std::string linkPhase = "mkdir -p \"$NIX_BUILD_TOP\"; ";
    linkPhase += fmt::format(
        "ln -s {} \"$NIX_BUILD_TOP/source\"; ",
        source_store_path);

    const std::string generatorName = target + ".drv";

    const fs::path nixja_gen_path = NIXJA_GEN_PATH;
    const std::string command = fmt::format("cd source && {} {}", nixja_gen_path.string(), target);

    const std::string buildPhase = std::format("NIXJA_GEN=$({}); echo $NIXJA_GEN;", command);
    const std::string installPhase = "cp \"$NIXJA_GEN\" \"$out\"; ";

    std::vector<std::string> inputSrcs;
    inputSrcs.push_back(fs::path(source_store_path).filename().string());
    if (nixja_gen_path.is_absolute())
        inputSrcs.push_back(nixja_gen_path.parent_path().parent_path().filename().string());
    inputSrcs.push_back(fs::path(NIX_STORE_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(NIX_UTIL_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(FMT_PACKAGE_PATH).filename().string());
    inputSrcs.push_back(fs::path(LLVM_PACKAGE_PATH).filename().string());

    json inputDrvs = json::object();

    const auto generator_drv_path = TRY(writeDerivation(store, {
                                                                   .name = generatorName,
                                                                   .system = "x86_64-linux",
                                                                   .linkPhase = linkPhase,
                                                                   .buildPhase = buildPhase,
                                                                   .installPhase = installPhase,
                                                                   .outputMethod = "text",
                                                                   .inputSrcs = inputSrcs,
                                                                   .inputDrvs = inputDrvs,
                                                               }));

    const auto generated_drv = nix::DownstreamPlaceholder::unknownCaOutput(generator_drv_path, "out");
    const auto generated_output = nix::DownstreamPlaceholder::unknownDerivation(generated_drv, "out");

    // This is the top-level derivation requested by nix-build. Its input is
    // the output of the derivation emitted by `generator`, not merely the
    // generator's output file.
    const json finalInputDrvs = {
        { std::string(generator_drv_path.to_string()), {
                                                           { "dynamicOutputs", {
                                                                                   { "out", {
                                                                                                { "dynamicOutputs", json::object() },
                                                                                                { "outputs", json::array({ "out" }) },
                                                                                            } },
                                                                               } },
                                                           { "outputs", json::array({ "out" }) },
                                                       } },
    };

    // Outputs nixja-target.drv
    const auto finalDrvPath = TRY(writeDerivation(store, {
                                                             .name = target,
                                                             .system = "x86_64-linux",
                                                             .linkPhase = "",
                                                             .buildPhase = "",
                                                             .installPhase = fmt::format("cp -r \"{}\" \"$out\"", generated_output.render()),
                                                             .outputMethod = "nar",
                                                             .inputSrcs = json::array(),
                                                             .inputDrvs = finalInputDrvs,
                                                         }));
    return std::string(finalDrvPath.to_string());
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
    const auto store_path = program.get<std::string>("--store");

    const auto drv_result = build_drv_file(target, store_path);
    MAIN_TRY(drv_result);
    const auto drv_path = std::move(drv_result.value());

    // Building this one derivation causes Nix to resolve the generator's
    // dynamic output and build the derivation it emits.
    // TODO: don't use nix-build (so we don't have to require recursive-nix in parent drv)
    // to solve this I might need to steal stuff from nix-ninja
    run_command({ "nix-build", "--quiet", "--extra-experimental-features", "ca-derivations dynamic-derivations", "--show-trace", "--out-link", "result", "/nix/store/" + drv_path });

    return 0;
}
