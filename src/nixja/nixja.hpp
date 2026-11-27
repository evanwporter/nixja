#pragma once

#include <filesystem>
#include <string>

#include "common.hpp"

Result<void> nixja(const std::string& target, const std::filesystem::path& requested_dir);
