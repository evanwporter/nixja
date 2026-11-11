#pragma once

#include <concepts>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fmt/format.h>
#include <nix/store/store-api.hh>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

// Move this Rustify stuff into a seperate github project
template <typename T, typename E = std::string>
using Result = std::expected<T, E>;

template <typename T>
struct IsResult : std::false_type { };

template <typename T, typename E>
struct IsResult<std::expected<T, E>> : std::true_type { };

inline std::unexpected<std::string> Err(std::string message) {
    return std::unexpected<std::string> { std::move(message) };
}

inline std::unexpected<std::string> Err(const std::string_view message) {
    return Err(std::string { message });
}

inline std::unexpected<std::string> Err(const char* message) {
    return Err(std::string_view { message });
}

template <typename T>
    requires(!std::convertible_to<T, std::string_view> && !IsResult<std::remove_cvref_t<T>>::value)
inline auto Err(T&& value) {
    return std::unexpected<std::decay_t<T>> { std::forward<T>(value) };
}

template <typename T, typename E>
inline auto Err(const std::expected<T, E>& result) {
    return std::unexpected<E> { result.error() };
}

template <typename T, typename E>
inline auto Err(std::expected<T, E>&& result) {
    return std::unexpected<E> { std::move(result.error()) };
}

template <typename... Args>
inline std::unexpected<std::string> Err(
    const fmt::format_string<Args...> format,
    Args&&... args) {
    return Err(fmt::format(format, std::forward<Args>(args)...));
}

// a Result or return its error
// https://www.reddit.com/r/cpp/comments/e1pdds/simplifying_stdexpected_error_handling_with_an/
#define TRY(expr) ({                                          \
    auto&& result = (expr);                                   \
    if (!result)                                              \
        return std::unexpected { std::move(result.error()) }; \
    std::move(result.value());                                \
})

struct DerivationSpec {
    std::string name;
    std::string system;

    /// Path to bash
    std::string builder;

    /// PATH env variable
    std::string path;

    std::string linkPhase;
    std::string buildPhase;
    std::string installPhase;

    /// `nar` or `text`
    std::string outputMethod;

    json inputSrcs;
    json inputDrvs;
    json extraEnv;
};

nix::StorePath writeDerivation(
    const nix::ref<nix::Store>& store,
    const DerivationSpec& spec);

std::string exec(const char* cmd);

Result<fs::path> parse_which(
    const std::string& cmd,
    const nix::ref<nix::Store>& store);
