#pragma once

// Nix defines a `debug(...)` macro that conflicts with spdlog.
#ifdef debug
#undef debug
#endif

#include <memory>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

inline void init_logging(
    const std::string& prefix,
    spdlog::level::level_enum level = spdlog::level::info) {

    auto logger = spdlog::stderr_color_mt(prefix);

    logger->set_pattern("[%n] [%^%l%$] %v");
    logger->set_level(level);

    spdlog::set_default_logger(std::move(logger));
}
