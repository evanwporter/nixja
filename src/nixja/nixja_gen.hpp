#include <string>

#include "common.hpp"

/// Create the buildable derivation for a Ninja target.  Depfile-producing
/// targets are wrapped so Nix follows their dynamic output.
Result<std::string> nixja_gen(const std::string& target);
