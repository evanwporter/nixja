#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <argparse/argparse.hpp>

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
#include "toolchain.hpp"

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
    const std::string generatorName = target + ".drv";

    const std::string command = fmt::format("cd source && {} {}", toolchain::nixja_gen, target);

    // These are the tools that must be on the path
    // const auto nixja_gen_path = TRY(parse_which("nixja-gen", store));
    // const auto nixja_scan_deps_path = TRY(parse_which("nixja-scan-deps", store));
    const auto coreutils_path = TRY(parse_which("cp", store));
    const auto cc_path = TRY(parse_which("cc", store));
    const auto clang_tools_path = TRY(parse_which("clang-scan-deps", store));

    // This is the scanning stage, not the compilation stage. Its output
    // is the derivation emitted by nixja-scan-deps; the derivation itself
    // builds the object files once Nix resolves the dynamic output.
    // This output is itself a derivation. A downstream derivation declares it
    // as a dynamic input, so Nix resolves and builds it in the same graph.
    const std::string installPhase = "cp \"$NIXJA_GEN\" \"$out\"; ";
    const std::string buildPhase = std::format(R"NIXJA(NIXJA_GEN=$({}); echo $NIXJA_GEN; )NIXJA", command);

    const char* bashBuilder = toolchain::bash;
    const char* buildPath = toolchain::build_path;

    std::vector<std::string> inputSrcs;
    inputSrcs.push_back(
        fs::path(source_store_path).filename().string());
    inputSrcs.push_back(
        toolchain::clang_tools_store_name);
    inputSrcs.push_back(toolchain::bash_store_name);
    inputSrcs.push_back(toolchain::coreutils_store_name);
    inputSrcs.push_back(toolchain::cc_store_name);
    if (toolchain::nixja_gen_store_name[0] != '\0')
        inputSrcs.push_back(toolchain::nixja_gen_store_name);

    // Libraries needed by nixja-gen
    inputSrcs.push_back(
        fs::path(toolchain::nix_store_package_path).filename().string());
    inputSrcs.push_back(
        fs::path(toolchain::nix_util_package_path).filename().string());
    inputSrcs.push_back(
        fs::path(toolchain::fmt_package_path).filename().string());

    json inputDrvs = json::object();

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
    };

    const auto finalDrvPath = writeDerivation(store, {
                                                         .name = target,
                                                         .system = "x86_64-linux",
                                                         .builder = bashBuilder,
                                                         .path = toolchain::coreutils_bin_dir,
                                                         .linkPhase = "",
                                                         .buildPhase = "",
                                                         .installPhase = "cp -r \"$input\" \"$out\"",
                                                         .outputMethod = "nar",
                                                         .inputSrcs = json::array({ toolchain::bash_store_name, toolchain::coreutils_store_name }),
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
    if (!drv_result) {
        std::cerr << drv_result.error() << '\n';
        return 1;
    }
    const auto drv_path = std::move(drv_result.value());

    // Building this one derivation causes Nix to resolve the generator's
    // dynamic output and build the derivation it emits.
    // TODO: don't use nix-build (so we don't have to require recursive-nix in parent drv)
    // to solve this I might need to steal stuff from nix-ninja
    run_command({ "nix-build", "--extra-experimental-features", "ca-derivations dynamic-derivations", "--show-trace", "--out-link", "result", "/nix/store/" + drv_path });

    return 0;
}
