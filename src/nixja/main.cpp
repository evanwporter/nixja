#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <nix/main/shared.hh>
#include <nix/store/derivations.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-dir-config.hh>
#include <nix/store/store-open.hh>
#include <nix/util/posix-source-accessor.hh>

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
std::string build_drv_file(const std::string name) {
    nix::experimentalFeatureSettings.experimentalFeatures.set(
        "ca-derivations dynamic-derivations");
    nix::initLibStore(true);
    const nix::ref<nix::Store> store = nix::openStore();

    const fs::path project_root = fs::canonical(fs::current_path());
    // Keep the complete project tree together.  The builder mounts this one
    // store path at $NIX_BUILD_TOP/source rather than recreating selected
    // files and directories individually.
    nix::PathFilter whole_project = [](const std::string&) {
        return true;
    };

    const auto stored = store->addToStore(
        "source",
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

    // A dynamic derivation output is parsed by Nix as a store derivation, so
    // its store-path name must carry the `.drv` suffix.  The outer result
    // derivation keeps the user-facing target name.
    const std::string generatorName = name + ".drv";
    const std::string derivationName = name;

    const std::string command = fmt::format("cd source && ./nixja-gen {}", name);

    // This is the scanning stage, not the compilation stage. Its output
    // is the derivation emitted by nixja-scan-deps; the derivation itself
    // builds the object files once Nix resolves the dynamic output.
    // This output is itself a derivation. A downstream derivation declares it
    // as a dynamic input, so Nix resolves and builds it in the same graph.
    const std::string installPhase = "cp \"$NIXJA_GEN\" \"$out\"; ";
    const std::string buildPhase = std::format(R"NIXJA(NIXJA_GEN=$({}); echo $NIXJA_GEN; )NIXJA", command);

    const char* bashBuilder = "/nix/store/nnm9gdr70ihc6zc9298mzz8jpcgaciql-bash-5.3p20/bin/bash";
    const char* buildPath = "/nix/store/g2ycks7l2dz7rr4l6cyxf4in7qrfjk8j-clang-tools-21.1.8/bin:/nix/store/cb987zkxvsv0490753ilrvn5yk7ylgxi-coreutils-9.12/bin:/nix/store/r5ri78axx1qggkjq3vawywlzglg70b5w-gcc-wrapper-16.2.0/bin";

    std::vector<std::string> inputSrcs;
    inputSrcs.push_back(
        fs::path(source_store_path).filename().string());
    inputSrcs.push_back(
        "g2ycks7l2dz7rr4l6cyxf4in7qrfjk8j-clang-tools-21.1.8");

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

    const auto generator_drv_path = writeDerivation(store, {
                                                               .name = generatorName,
                                                               .system = "x86_64-linux",
                                                               .builder = bashBuilder,
                                                               .path = buildPath,
                                                               .linkPhase = linkPhase,
                                                               .buildPhase = buildPhase,
                                                               .installPhase = installPhase,
                                                               .outputMethod = "text",
                                                               .inputSrcs = inputSrcs,
                                                               .inputDrvs = inputDrvs,
                                                               .extraEnv = json::object(),
                                                           });

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
        { "30hr2syjmfvflwi68acggp3w870ikw5c-bash-5.3p20.drv", {
                                                                  { "dynamicOutputs", json::object() },
                                                                  { "outputs", json::array({ "out" }) },
                                                              } },
        { "m7bmd2qz1zcnk4k25y5cg6wbb3y9qwcb-coreutils-9.12.drv", {
                                                                     { "dynamicOutputs", json::object() },
                                                                     { "outputs", json::array({ "out" }) },
                                                                 } },
    };

    const auto finalDrvPath = writeDerivation(store, {
                                                         .name = derivationName,
                                                         .system = "x86_64-linux",
                                                         .builder = bashBuilder,
                                                         .path = "/nix/store/cb987zkxvsv0490753ilrvn5yk7ylgxi-coreutils-9.12/bin",
                                                         .linkPhase = "",
                                                         .buildPhase = "",
                                                         .installPhase = "cp -r \"$input\" \"$out\"",
                                                         .outputMethod = "nar",
                                                         .inputSrcs = json::array(),
                                                         .inputDrvs = finalInputDrvs,
                                                         .extraEnv = {
                                                             { "input", generated_output.render() },
                                                         },
                                                     });
    return std::string(finalDrvPath.to_string());
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

        const std::string target = argv[1];

        const auto drv_path = build_drv_file(target);

        // Building this one derivation causes Nix to resolve the generator's
        // dynamic output and build the derivation it emits.
        // TODO: don't use nix-build (so we don't have to require recursive-nix in parent drv)
        // to solve this I might need to steal stuff from nix-ninja
        // TODO: use NIXPATH to obtain `pkgs`
        run_command({ "nix-build", "--extra-experimental-features", "ca-derivations dynamic-derivations", "--show-trace", "--out-link", "result", "/nix/store/" + drv_path });

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
