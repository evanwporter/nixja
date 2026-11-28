#include <cstdint>
#include <filesystem>
#include <string>

#include <clang/Driver/Driver.h>
#include <llvm/ADT/SmallVector.h>
#include <tree_sitter/api.h>
#include <unordered_set>

#include "common.hpp"

namespace fs = std::filesystem;

struct Command {
    /// The command to run
    std::string command;

    /// A set of every input src (into the nix store) that this command requires
    std::unordered_set<std::string> inputSrcs;
};

struct CommandInvocation {
    std::string_view program;
    std::string_view command;
    uint32_t program_offset;
};

bool is_compile_command(clang::driver::Driver& driver, llvm::ArrayRef<const char*> args);

/**
 * @brief Generates a Nix derivation for a target in a Ninja build graph.
 *
 * Create the buildable derivation for a Ninja target. Depfile-producing
 * targets are wrapped so Nix follows their dynamic output.
 *
 * @param target The Ninja build target to generate a derivation for.
 * @param source_root The directory where `nixja` is called from.
 * @param build_dir The directory containing the build.ninja file.
 * @return The Nix store path of the generated derivation
 */
Result<std::string> nixja_gen(const std::string& target, const fs::path& source_root, const fs::path& build_dir);

void collect_commands(const TSNode node, const std::string_view source, std::vector<CommandInvocation>& commands);

/// Given a generic NINJA command, we parse all the possible NIX STORE paths from it
/// We also rework the program (the first arg in the command), so that it complies.
///
/// For example if the program is an absolute path under source_root, then we add the path
/// to the store and replace the program with the path from the store.
/// TODO: Parse the complex shell syntax like program1 && program2
/// TODO: Rework to flow better & DRY
Result<Command> parseCommand(
    const std::string& command,
    const fs::path& source_root,
    const std::string& source_store_path,
    const nix::ref<nix::Store> store);
