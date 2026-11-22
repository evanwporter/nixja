#include "common.hpp"

#include <filesystem>
#include <stdexcept>

#include <fmt/format.h>
#include <nix/store/derivations.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/util/source-accessor.hh>

namespace fs = std::filesystem;

std::string shell_quote(const std::string_view value) {
    std::string quoted = "'";
    for (const char ch : value)
        quoted += ch == '\'' ? "'\\''" : std::string(1, ch);
    quoted += '\'';
    return quoted;
}

std::string nixja_store_name(const std::string_view name, const std::string_view prefix) {
    const std::string prefix_with_separator = std::string(prefix) + "-";
    if (name.starts_with(prefix_with_separator))
        return std::string(name);

    return prefix_with_separator + std::string(name);
}

std::string make_task_command(const TaskSpec& spec) {
    std::string command = "mkdir -p \"$NIX_BUILD_TOP\"; ";
    if (spec.source_store_path) {
        command += fmt::format(
            "(cd {}; find . -type d -exec mkdir -p \"$NIX_BUILD_TOP/{{}}\" \\; ; "
            "find . ! -type d -exec ln -s \"{}/{{}}\" \"$NIX_BUILD_TOP/{{}}\" \\;); ",
            shell_quote(spec.source_store_path->string()),
            spec.source_store_path->string());
    }
    for (const auto& input : spec.inputs) {
        const auto destination = input.file_relative.generic_string();
        command += fmt::format(
            "mkdir -p \"$NIX_BUILD_TOP/{}\"; rm -f \"$NIX_BUILD_TOP/{}\"; ln -s \"{}\" \"$NIX_BUILD_TOP/{}\"; ",
            input.file_relative.parent_path().generic_string(), destination,
            input.source_store_path.string(), destination);
    }
    command += fmt::format("mkdir -p \"$NIX_BUILD_TOP/{}\"; cd \"$NIX_BUILD_TOP/{}\"; ", spec.build_dir.generic_string(), spec.build_dir.generic_string());
    for (const auto& output : spec.outputs) {
        const auto parent = output.parent_path();
        if (!parent.empty())
            command += fmt::format("mkdir -p {}; ", shell_quote(parent.generic_string()));
    }
    command += fmt::format("bash -e -c {}; ", shell_quote(spec.command));
    if (!spec.outputs.empty()) {
        command += "mkdir -p \"$out\"; ";
        for (const auto& output : spec.outputs) {
            const auto parent = output.parent_path();
            if (!parent.empty())
                command += fmt::format("mkdir -p \"$out/{}\"; ", parent.generic_string());
            command += fmt::format("cp {} \"$out/{}\"; ", shell_quote(output.generic_string()), output.generic_string());
        }
    }
    return command;
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
    const auto cxx = TRY(parse_which("c++", store));
    const auto clang_tools = TRY(parse_which("clang-scan-deps", store));

    // HACK: This should be combined with cc/cxx above or handled in parseCommand
    const auto clang = TRY(parse_which("clang", store));

    const auto find = TRY(parse_which("find", store));

    const std::string core_path = fmt::format(
        "{}/bin:{}/bin:{}/bin:{}/bin:{}/bin:{}/bin:{}/bin:{}/bin",
        bash.string(),
        coreutils.string(),
        which.string(),
        cc.string(),
        cxx.string(),
        clang.string(),
        clang_tools.string(),
        find.string());

    json env = spec.extraEnv;
    env["name"] = derivation_name;
    env["PATH"] = spec.path.empty() ? core_path : core_path + ":" + spec.path;
    env["outputs"] = "out";
    env["requiredSystemFeatures"] = "recursive-nix";
    env["out"] = nix::hashPlaceholder("out");

    json inputSrcs = json::array({
        bash.filename().string(),
        coreutils.filename().string(),
        which.filename().string(),
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
            json::array({ "-e", "-c", spec.buildCommand }),
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

std::string add_file_to_store_with_parents(
    const nix::ref<nix::Store>& store,
    const fs::path& source_root,
    const fs::path& file_relative,
    std::string_view name_prefix) {

    // The filter sees paths rooted at /, regardless of project_root's location.
    const std::string wanted = (fs::path("/") / file_relative).generic_string();
    nix::PathFilter filter = [wanted](const std::string& path) {
        return path == wanted || (path.size() < wanted.size() && wanted.compare(0, path.size(), path) == 0 && (path == "/" || wanted[path.size()] == '/'));
    };

    const auto stored = store->addToStore(
        nixja_store_name(file_relative.filename().string(), name_prefix),
        nix::makeFSSourceAccessor(source_root),
        nix::ContentAddressMethod::Raw::NixArchive,
        nix::HashAlgorithm::SHA256,
        nix::StorePathSet { },
        filter);

    return store->printStorePath(stored);
}

/// Outputs everything after the `--` in the command
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
