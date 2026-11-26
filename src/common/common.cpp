#include "common.hpp"

#include <cctype>
#include <filesystem>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <nix/store/derivations.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>

namespace fs = std::filesystem;

std::string nixja_store_name(const std::string_view name, const std::string_view prefix) {
    std::string result(name);
    for (char& c : result) {
        const auto ch = static_cast<unsigned char>(c);
        if (!std::isalnum(ch) && std::string_view("+-._?=").find(c) == std::string_view::npos)
            c = '_';
    }
    const std::string prefix_with_separator = std::string(prefix) + "-";
    if (result.starts_with(prefix_with_separator))
        return result;
    return prefix_with_separator + result;
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
    const auto patchelf = TRY(parse_which("patchelf", store));
    const auto cc = TRY(parse_which("cc", store));
    const auto cxx = TRY(parse_which("c++", store));
    const auto clang_tools = TRY(parse_which("clang-scan-deps", store));

    // HACK: This should be combined with cc/cxx above or handled in parseCommand
    const auto clang = TRY(parse_which("clang", store));

    const auto find = TRY(parse_which("find", store));

    const std::string core_path = fmt::format(
        "{}/bin:{}/bin:{}/bin:{}/bin:{}/bin:{}/bin:{}/bin:{}/bin:{}/bin",
        bash.string(),
        coreutils.string(),
        which.string(),
        patchelf.string(),
        cc.string(),
        cxx.string(),
        clang.string(),
        clang_tools.string(),
        find.string());

    json env = spec.extraEnv;
    env["name"] = derivation_name;
    env["PATH"] = spec.path.empty() ? core_path : core_path + ":" + spec.path;
    env["linkPhase"] = spec.linkPhase;
    env["buildPhase"] = spec.buildPhase;
    env["installPhase"] = spec.installPhase;
    env["outputs"] = fmt::format("{}", fmt::join(spec.outputs, " "));
    env["requiredSystemFeatures"] = "recursive-nix";
    json outputs = json::object();
    for (const auto& name : spec.outputs) {
        env[name] = nix::hashPlaceholder(name);
        outputs[name] = { { "method", spec.outputMethod }, { "hashAlgo", "sha256" } };
    }

    json inputSrcs = json::array({
        bash.filename().string(),
        coreutils.filename().string(),
        which.filename().string(),
        patchelf.filename().string(),
        cc.filename().string(),
        cxx.filename().string(),
        clang.filename().string(),
        clang_tools.filename().string(),
        find.filename().string(),
    });

    for (const auto& input : spec.inputSrcs)
        inputSrcs.push_back(input);

    const json drv = {
        { "name", derivation_name },
        /// TODO: other symstems
        { "system", "x86_64-linux" },
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
        { "outputs", outputs },
    };

    return nix::Derivation::parseJsonAndValidate(*store, drv);
}

Result<nix::StorePath> writeDerivation(
    const nix::ref<nix::Store>& store,
    const DerivationSpec& spec) {

    const auto derivation = TRY(buildDerivation(store, spec));
    return store->writeDerivation(derivation);
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
        return Err("Could not parse '{}' for '{}' as a store path: {}", path.string(), cmd, error.what());
    }

    return path;
}

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

static std::string shell_quote(std::string_view value) {
    std::string result = "'";
    for (char c : value)
        result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}

std::string make_task_command(const TaskSpec& spec, const fs::path& task_command) {
    std::string inputs;
    for (const auto& input : spec.inputs)
        inputs += shell_quote(input.source_store_path.string() + ":" + input.file_relative.string()) + " ";

    std::string outputs;
    for (const auto& output : spec.outputs)
        outputs += shell_quote(output.string()) + " ";

    const std::string input_directory = spec.source_store_path.has_value()
        ? " -d " + shell_quote(spec.source_store_path->string())
        : "";
    return fmt::format("{} -i {} -o {}{} -C {} -- {}", shell_quote(task_command.string()), inputs, outputs, input_directory, shell_quote(spec.build_relative.string()), shell_quote(spec.command));
}
