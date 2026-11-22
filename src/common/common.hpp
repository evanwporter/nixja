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

template <typename T, typename E>
decltype(auto) unwrap_result(std::expected<T, E>& result) {
    if constexpr (std::is_void_v<T>) {
        result.value();
    } else {
        return std::move(result.value());
    }
}

// a Result or return its error
// Equivalent to `?` in RUST
// https://www.reddit.com/r/cpp/comments/e1pdds/simplifying_stdexpected_error_handling_with_an/
#define TRY(expr) ({                                                              \
    auto&& result = (expr);                                                       \
    if (!result)                                                                  \
        return Err(fmt::format("{}:{}: {}", __FILE__, __LINE__, result.error())); \
    unwrap_result(result);                                                        \
})

// Propagate an expected_fs result into a Result with a string error.
#define TRY_EC(expr) ({                                                                     \
    auto&& result = (expr);                                                                 \
    if (!result)                                                                            \
        return Err(fmt::format("{}:{}: {}", __FILE__, __LINE__, result.error().message())); \
    unwrap_result(result);                                                                  \
})

/// Use only from an int-returning main function.
#define MAIN_TRY(expr)                           \
    do {                                         \
        auto&& result = (expr);                  \
        if (!result) {                           \
            std::cerr << result.error() << '\n'; \
            return 1;                            \
        }                                        \
    } while (false)

struct DerivationSpec {
    std::string name;
    /// Namespace for store objects created by the executable producing this derivation.
    std::string storeNamePrefix = "nixja";

    /// Additional PATH entries, appended after the core packages.
    std::string path;

    /// TODO: set defaults

    std::string linkPhase;
    std::string buildPhase;
    std::string installPhase;

    /// `nar` or `text`
    std::string outputMethod;

    json inputSrcs = json::array();
    json inputDrvs = json::object();

    /// TODO: Currently this is unused
    json extraEnv;
};

struct StoreFile {
    /// The path of the file in the nix store
    std::filesystem::path source_store_path;

    /// The path of the the file relative to the source root
    std::filesystem::path file_relative;
};

struct TaskSpec {
    std::vector<StoreFile> inputs;

    std::vector<std::filesystem::path> outputs;

    /// The source_root directory in the nix store.
    std::optional<std::filesystem::path> source_store_path;

    /// The relative build directory which command is run from
    std::filesystem::path build_relative = ".";

    /// The command to run
    std::string command;
};

/// Render the Bash builder command for a TaskSpec.
std::string make_task_command(const TaskSpec& spec, const std::filesystem::path& task_command);

/// Build a derivation and write it to the nix store. The return is the path in the store.
/// TODO: fold this into buildDerivation
Result<nix::StorePath> writeDerivation(
    const nix::ref<nix::Store>& store,
    const DerivationSpec& spec);

std::string nixja_store_name(std::string_view name, std::string_view prefix);

/// Execute shell command and capture stdout
// Source - https://stackoverflow.com/a/478960
// Posted by waqas, modified by community. See post 'Timeline' for change history
// Retrieved 2026-10-31, License - CC BY-SA 4.0
std::string exec(const char* cmd);

Result<std::filesystem::path> parse_which(
    const std::string& cmd,
    const nix::ref<nix::Store>& store);

/// Outputs everything after the `--` in the command
/// TODO: Combine more efficiently with other cmd line parsing
std::string arguments_after_separator(int argc, char* argv[]);

/// Add local dependencies to the Nix store, preserving their paths
/// relative to the source root.
template <std::ranges::input_range Range>
std::vector<StoreFile> add_dependencies_to_store(
    const Range& dependencies,
    const nix::ref<nix::Store>& store,
    const std::filesystem::path& source_root,
    const std::filesystem::path& dependency_root) {

    std::vector<StoreFile> store_paths;

    if constexpr (std::ranges::sized_range<const Range>)
        store_paths.reserve(std::ranges::size(dependencies));

    for (const auto& dependency : dependencies) {
        const std::filesystem::path dependency_path = [&] {
            if constexpr (requires { dependency->path(); })
                return std::filesystem::path(dependency->path());
            else
                return std::filesystem::path(dependency);
        }();
        const std::filesystem::path absolute = dependency_path.is_absolute()
            ? dependency_path.lexically_normal()
            : (dependency_root / dependency_path).lexically_normal();

        const std::filesystem::path file_relative = absolute.lexically_relative(source_root);
        if (file_relative.empty() || file_relative.is_absolute()
            || (!file_relative.empty() && *file_relative.begin() == ".."))
            continue;

        // Ninja's phony nodes (for example, `PHONY`) are graph markers, not
        // files that can be copied into the store.
        if (!std::filesystem::exists(absolute))
            continue;

        const std::filesystem::path file = std::filesystem::canonical(absolute);

        const auto stored = store->addToStore(
            nixja_store_name(file.filename().string(), "nixja-scan-deps"),
            nix::makeFSSourceAccessor(file),
            nix::ContentAddressMethod::Raw::Flat);

        store_paths.emplace_back(store->printStorePath(stored), file_relative);
    }

    return store_paths;
}
