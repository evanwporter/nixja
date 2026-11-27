#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <nix/store/store-api.hh>
#include <nix/util/source-accessor.hh>

#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include "common.hpp"
#include "nixja.hpp"
#include "nixja_gen.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

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
Result<void> nixja(const std::string& target, const fs::path& requested_dir) {
    /// The directory where nixja is called from, and the directory that we
    /// add to the store as `source`
    const fs::path source_root = fs::current_path();

    std::error_code ec;

    /// The directory that becomes we run ninja commands from and is the absolute path of the build graph.
    /// Also the directory where the `build.ninja` file is located.
    const fs::path build_dir = fs::canonical(source_root / requested_dir, ec);

    if (ec)
        return Err("Invalid build directory specified with -C {}: {}", requested_dir.string(), ec.message());

    /// The build directory relative to the source_root
    const fs::path build_relative = build_dir.lexically_relative(source_root);

    // Make sure that passed build_dir is inside the source_root
    if (build_relative.empty() || build_relative.is_absolute() || (*build_relative.begin() == ".."))
        return Err("Build directory must be inside the source root");

    const auto drv_path = TRY(nixja_gen(target, source_root, build_dir));

    // Building this one derivation causes Nix to resolve the generator's
    // dynamic output and build the derivation it emits.
    run_command({ "nix", "build", "--print-build-logs", "--extra-experimental-features", "ca-derivations dynamic-derivations", "--show-trace", "--out-link", "result", drv_path + "^out" });

    return { };
}
