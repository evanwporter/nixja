#include <filesystem>
#include <string>

#include "common.hpp"

namespace fs = std::filesystem;

/// Create the buildable derivation for a Ninja target.  Depfile-producing
/// targets are wrapped so Nix follows their dynamic output.
Result<std::string> nixja_gen(const std::string& target, const fs::path& source_root, const fs::path& build_dir);
