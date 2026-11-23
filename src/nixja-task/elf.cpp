#include <cstdint>
#include <cstring>
#include <fstream>

#include <elf.h>

#include <expected_fs/expected_fs.hpp>

#include "elf.hpp"

namespace fs = expected_fs;

bool is_patchelf_compatible(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;

    unsigned char header[EI_NIDENT + sizeof(std::uint16_t)];
    if (!file.read(reinterpret_cast<char*>(header), sizeof(header)))
        return false;

    if (std::memcmp(header, ELFMAG, SELFMAG) != 0)
        return false;

    std::uint16_t type = 0;
    if (header[EI_DATA] == ELFDATA2LSB)
        type = static_cast<std::uint16_t>(header[EI_NIDENT])
            | (static_cast<std::uint16_t>(header[EI_NIDENT + 1]) << 8);
    else if (header[EI_DATA] == ELFDATA2MSB)
        type = (static_cast<std::uint16_t>(header[EI_NIDENT]) << 8)
            | static_cast<std::uint16_t>(header[EI_NIDENT + 1]);
    else
        return false;

    return type == ET_EXEC || type == ET_DYN;
}
