#include <elf.h>

#include <expected_fs/expected_fs.hpp>

namespace fs = expected_fs;

bool is_patchelf_compatible(const fs::path& path);
