// Shared Updater Server - include/updater/catalog.hpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#pragma once

#include "updater/binary.hpp"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <flat_map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace updater {

struct Timestamp {
    std::chrono::sys_seconds seconds{};
    std::chrono::nanoseconds fraction{};

    auto operator<=>(const Timestamp&) const = default;
};

struct CatalogHeader {
    std::int64_t schema_version = 1;
    std::string revision;
    Timestamp published_at;
    std::string author;
    bool operator==(const CatalogHeader&) const = default;
};

struct Suite {
    std::string title;
    std::vector<std::string> packages;
    bool operator==(const Suite&) const = default;
};

struct Package {
    std::string title;
    std::string kind;
    bool operator==(const Package&) const = default;
};

struct BuildKey {
    std::string package;
    std::string id;

    auto operator<=>(const BuildKey&) const = default;
};

struct Artifact {
    std::string url;
    std::string media_type;
    std::uint64_t size = 0;
    Sha256 sha256{};
    bool operator==(const Artifact&) const = default;
};

struct Extension {
    std::string namespace_uri;
    std::string name;
    std::string text;
    std::optional<std::string> suite;
    bool operator==(const Extension&) const = default;
};

enum class Visibility { public_build, targeted };

struct Build {
    std::string version;
    Timestamp published_at;
    std::string about_url;
    Visibility visibility = Visibility::public_build;
    std::vector<std::string> requirements;
    std::flat_map<std::string, Artifact, std::less<>> artifacts;
    std::vector<Extension> extensions;
    bool operator==(const Build&) const = default;
};

struct TrackKey {
    std::string suite;
    std::string package;
    std::string name;
    std::string platform;
    std::optional<Uuid> installation_id;

    auto operator<=>(const TrackKey&) const = default;
};

struct CatalogUsage {
    std::ptrdiff_t bytes = 0;
    std::ptrdiff_t fragments = 0;
};

inline constexpr CatalogUsage catalog_limits{
    .bytes = 8 * 1024 * 1024,
    .fragments = 4096
};

struct Catalog {
    CatalogHeader header;
    std::flat_map<std::string, Suite, std::less<>> suites;
    std::flat_map<std::string, Package, std::less<>> packages;
    std::flat_map<BuildKey, Build> builds;
    std::flat_map<TrackKey, std::string> tracks;
    CatalogUsage usage;
    CatalogUsage limits = catalog_limits;
};

struct SourceLocation {
    std::filesystem::path file;
    std::ptrdiff_t line = 0;
    std::ptrdiff_t column = 0;
};

enum class DiagnosticKind { input, limit, syntax, schema, duplicate, reference };

struct Diagnostic {
    DiagnosticKind kind;
    SourceLocation location;
    std::string message;
    std::optional<SourceLocation> related;
};

using CatalogLoad = std::expected<std::unique_ptr<const Catalog>, std::vector<Diagnostic>>;

// A load returns a complete snapshot or diagnostics. Callers choose when to
// activate it, so validating a replacement cannot change the live catalog.
[[nodiscard]] CatalogLoad load_catalog(
    std::span<const std::filesystem::path> fragments, CatalogUsage limits = catalog_limits);
[[nodiscard]] CatalogLoad load_catalog_directory(
    const std::filesystem::path& directory, CatalogUsage limits = catalog_limits);
[[nodiscard]] std::string format_diagnostic(const Diagnostic& diagnostic);
[[nodiscard]] std::string format_success(const Catalog& catalog);

} // namespace updater
