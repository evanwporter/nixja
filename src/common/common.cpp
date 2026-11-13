#include "common.hpp"

#include <filesystem>

#include <fmt/format.h>
#include <nix/store/derivations.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>

namespace fs = std::filesystem;

std::string nixja_store_name(const std::string_view name, const std::string_view prefix) {
    const std::string prefix_with_separator = std::string(prefix) + "-";
    if (name.starts_with(prefix_with_separator))
        return std::string(name);

    return prefix_with_separator + std::string(name);
}

/// TODO: think about moving this out of the header file
Result<nix::Derivation> buildDerivation(
    const nix::ref<nix::Store>& store,
    const DerivationSpec& spec) {

    const std::string derivation_name = nixja_store_name(spec.name, spec.storeNamePrefix);

    /// Core packages which every nixja* must have at access to at runtime
    const auto bash = TRY(parse_which("bash", store));
    const auto coreutils = TRY(parse_which("cp", store));
    const auto which = TRY(parse_which("which", store));
    const auto cc = TRY(parse_which("cc", store));
    const auto clang_tools = TRY(parse_which("clang-scan-deps", store));

    const std::string core_path = fmt::format(
        "{}/bin:{}/bin:{}/bin:{}/bin:{}/bin",
        bash.string(),
        coreutils.string(),
        which.string(),
        cc.string(),
        clang_tools.string());

    json env = spec.extraEnv;
    env["name"] = derivation_name;
    env["PATH"] = spec.path.empty() ? core_path : core_path + ":" + spec.path;
    env["linkPhase"] = spec.linkPhase;
    env["buildPhase"] = spec.buildPhase;
    env["installPhase"] = spec.installPhase;
    env["outputs"] = "out";
    env["requiredSystemFeatures"] = "recursive-nix";
    env["out"] = nix::hashPlaceholder("out");

    json inputSrcs = json::array({
        bash.filename().string(),
        coreutils.filename().string(),
        which.filename().string(),
        cc.filename().string(),
        clang_tools.filename().string(),
    });

    for (const auto& input : spec.inputSrcs)
        inputSrcs.push_back(input);

    const json drv = {
        { "name", derivation_name },
        { "system", spec.system },
        { "builder", (bash / "bin" / "bash").string() },
        { "version", 4u },
        {
            "args",
            // TODO: Only append the phase var if its not empty
            json::array({ "-e", "-c", "eval \"$linkPhase\"; eval \"$buildPhase\"; eval \"$installPhase\"" }),
        },
        { "env", env },
        { "inputs", {
                        { "srcs", inputSrcs },
                        { "drvs", spec.inputDrvs },
                    } },
        { "outputs", {
                         { "out", {
                                      { "method", spec.outputMethod },
                                      { "hashAlgo", "sha256" },
                                  } },
                     } },
    };

    return nix::Derivation::parseJsonAndValidate(*store, drv);
}

Result<nix::StorePath> writeDerivation(
    const nix::ref<nix::Store>& store,
    const DerivationSpec& spec) {

    const auto derivation = TRY(buildDerivation(store, spec));
    return store->writeDerivation(derivation);
}

Result<std::string> emitDerivation(
    const nix::ref<nix::Store>& store,
    const DerivationSpec& spec) {

    const auto derivation = TRY(buildDerivation(store, spec));
    return derivation.unparse(*store, false);
}

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
