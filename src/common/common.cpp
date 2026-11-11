#include "common.hpp"

#include <expected>
#include <filesystem>

#include <fmt/format.h>
#include <nix/store/derivations.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>

namespace fs = std::filesystem;

nix::StorePath writeDerivation(
    const nix::ref<nix::Store>& store,
    const DerivationSpec& spec) {

    json env = spec.extraEnv;
    env["name"] = spec.name;
    env["PATH"] = spec.path;
    env["linkPhase"] = spec.linkPhase;
    env["buildPhase"] = spec.buildPhase;
    env["installPhase"] = spec.installPhase;
    env["outputs"] = "out";
    env["requiredSystemFeatures"] = "recursive-nix";
    env["out"] = nix::hashPlaceholder("out");

    const json drv = {
        { "name", spec.name },
        { "system", spec.system },
        { "builder", spec.builder },
        { "version", 4u },
        {
            "args",
            // TODO: Only append the phase var if its not empty
            json::array({ "-e", "-c", "eval \"$linkPhase\"; eval \"$buildPhase\"; eval \"$installPhase\"" }),
        },
        { "env", env },
        { "inputs", {
                        { "srcs", spec.inputSrcs },
                        { "drvs", spec.inputDrvs },
                    } },
        { "outputs", {
                         { "out", {
                                      { "method", spec.outputMethod },
                                      { "hashAlgo", "sha256" },
                                  } },
                     } },
    };

    const auto derivation = nix::Derivation::parseJsonAndValidate(*store, drv);

    return store->writeDerivation(derivation);
}

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

Result<fs::path> parse_which(
    const std::string& cmd,
    const nix::ref<nix::Store>& store) {
    // TODO: check if path exists
    // also just better error handling

    std::string output = exec(fmt::format("which {}", cmd).c_str());
    const auto end = output.find_last_not_of(" \t\r\n");
    if (end == std::string::npos)
        return Err("Could not find '{}' on PATH.", cmd);
    output.erase(end + 1);

    const auto path = fs::path(output).parent_path().parent_path();
    try {
        const auto store_path = store->parseStorePath(path.string());
        if (!store->isValidPath(store_path))
            return Err("The store path: '{}' does not lie in the Nix store.", store_path.to_string());
    } catch (const std::exception& error) {
        return Err("Could not parse '{}' as a store path: {}", path.string(), error.what());
    }

    return path;
}
