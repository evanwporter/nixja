#include <filesystem>
#include <string>

#include "common.hpp"

namespace fs = std::filesystem;

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
