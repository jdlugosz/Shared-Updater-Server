// Shared Updater Server - src/catalog.cpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#include "updater/catalog.hpp"
#include "updater/identifier.hpp"
#include "updater/xml.hpp"
#include "updater/uri.hpp"

#include <toml++/toml.hpp>

#include <arpa/inet.h>

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <algorithm>
#include <charconv>
#include <fstream>
#include <format>
#include <initializer_list>
#include <map>
#include <ranges>
#include <string_view>
#include <utility>
#endif

namespace updater {
namespace {

namespace fs = std::filesystem;
using namespace std::string_view_literals;

constexpr auto core_namespace = "https://dlugosz.enterprises/ns/updater/1"sv;
constexpr auto atom_namespace = "http://www.w3.org/2005/Atom"sv;
constexpr auto xml_namespace = "http://www.w3.org/XML/1998/namespace"sv;
constexpr auto xmlns_namespace = "http://www.w3.org/2000/xmlns/"sv;

// helpers for media_type
bool digit(char c) { return c >= '0' && c <= '9'; }
bool lower(char c) { return c >= 'a' && c <= 'z'; }
bool alpha(char c) { return lower(c) || (c >= 'A' && c <= 'Z'); }
bool alnum(char c) { return alpha(c) || digit(c); }

bool media_type(std::string_view type)
{
    const auto slash = type.find('/');
    const auto token = [](std::string_view part) {
        return !part.empty() && std::ranges::all_of(part, [](char c) {
            return alnum(c) || c == '`' || "!#$%&'*+-.^_|~"sv.contains(c);
        });
    };
    return slash != type.npos && token(type.subview(0, slash)) && token(type.subview(slash + 1));
}

SourceLocation at(const toml::source_region& region)
{
    return {region.path ? fs::path(*region.path) : fs::path{}, region.begin.line, region.begin.column};
}

template<class T>
struct Defined {
    T value;
    SourceLocation location;
};

template<class Key, class Value, class Compare>
std::flat_map<Key, Value, Compare> take_values(std::map<Key, Defined<Value>, Compare>& definitions)
{
    std::vector<Key> keys;
    std::vector<Value> values;
    keys.reserve(definitions.size());
    values.reserve(definitions.size());
    while (!definitions.empty()) {
        auto node = definitions.extract(definitions.begin());
        keys.push_back(std::move(node.key()));
        values.push_back(std::move(node.mapped().value));
    }
    return {std::sorted_unique, std::move(keys), std::move(values), definitions.key_comp()};
}

struct InvalidRecord {};

class Loader {
public:
    CatalogLoad load(std::span<const fs::path> fragments, CatalogUsage limits);

private:
    std::vector<Diagnostic> diagnostics_;
    std::optional<Defined<CatalogHeader>> header_;
    std::map<std::string, Defined<Suite>, std::less<>> suites_;
    std::map<std::string, Defined<Package>, std::less<>> packages_;
    std::map<BuildKey, Defined<Build>> builds_;
    std::map<TrackKey, Defined<std::string>> tracks_;
    std::map<BuildKey, std::vector<SourceLocation>> extension_locations_;

    void report(DiagnosticKind kind, SourceLocation location, std::string message,
                std::optional<SourceLocation> related = {});
    [[noreturn]] void invalid(const toml::node& node, std::string message);
    void keys(const toml::table& table, std::initializer_list<std::string_view> allowed);
    const toml::node& required(const toml::table& table, std::string_view field);
    std::string text(const toml::node& node, std::string_view field, std::ptrdiff_t limit,
                     bool empty = false);
    std::string id(const toml::node& node, std::string_view field, bool suite = false);
    std::string link(const toml::table& table, std::string_view field);
    std::int64_t integer(const toml::node& node, std::string_view field);
    Timestamp timestamp(const toml::table& table, std::string_view field);
    std::vector<std::string> ids(const toml::node& node, std::string_view field);

    template<class Map, class Key, class Value>
    bool define(Map& map, Key key, Value value, const toml::node& node, std::string description);
    template<class Read>
    void records(const toml::table& table, std::string_view field, Read read);

    void read_fragment(const toml::table& root);
    void read_header(const toml::node& node);
    void read_suite(const toml::table& table);
    void read_package(const toml::table& table);
    void read_build(const toml::table& table);
    void read_track(const toml::table& table);
    void validate_references();
    std::unique_ptr<const Catalog> finish(CatalogUsage usage, CatalogUsage limits);
};

void Loader::report(DiagnosticKind kind, SourceLocation location, std::string message,
                    std::optional<SourceLocation> related)
{
    constexpr std::ptrdiff_t cap = 100;
    if (std::ssize(diagnostics_) < cap)
        diagnostics_.push_back({kind, std::move(location), std::move(message), std::move(related)});
    else if (std::ssize(diagnostics_) == cap)
        diagnostics_.push_back({DiagnosticKind::limit, std::move(location),
                                "further catalog diagnostics omitted", {}});
}

[[noreturn]] void Loader::invalid(const toml::node& node, std::string message)
{
    report(DiagnosticKind::schema, at(node.source()), std::move(message));
    throw InvalidRecord{};
}

void Loader::keys(const toml::table& table, std::initializer_list<std::string_view> allowed)
{
    for (const auto& [key, value] : table) {
        if (!std::ranges::contains(allowed, key.str()))
            report(DiagnosticKind::schema, at(key.source()), std::format("unknown field '{}'", key.str()));
    }
}

const toml::node& Loader::required(const toml::table& table, std::string_view field)
{
    if (const auto* value = table.get(field)) return *value;
    invalid(table, std::format("missing required field '{}'", field));
}

std::string Loader::text(const toml::node& node, std::string_view field, std::ptrdiff_t limit, bool empty)
{
    const auto* value = node.as_string();
    if (!value) invalid(node, std::format("'{}' must be a string", field));
    const auto& str = value->get();
    if (!empty && str.empty()) invalid(node, std::format("'{}' must not be empty", field));
    if (std::ssize(str) > limit) invalid(node, std::format("'{}' exceeds {} bytes", field, limit));
    if (!xml::valid_text(str)) invalid(node, std::format("'{}' contains text that XML 1.0 cannot represent", field));
    return str;
}

std::string Loader::id(const toml::node& node, std::string_view field, bool suite)
{
    auto value = text(node, field, 64);
    if (!valid_catalog_identifier(value, suite))
        invalid(node, std::format("'{}' must be a canonical lowercase identifier", field));
    return value;
}

std::string Loader::link(const toml::table& table, std::string_view field)
{
    const auto& node = required(table, field);
    auto value = text(node, field, 2048);
    if (!valid_https_url(value)) invalid(node, std::format("'{}' must be an absolute HTTPS URL", field));
    return value;
}

std::int64_t Loader::integer(const toml::node& node, std::string_view field)
{
    if (const auto* value = node.as_integer()) return value->get();
    invalid(node, std::format("'{}' must be an integer", field));
}

Timestamp Loader::timestamp(const toml::table& table, std::string_view field)
{
    using namespace std::chrono;
    const auto& node = required(table, field);
    const auto* value = node.as_date_time();
    if (!value || !value->get().offset)
        invalid(node, std::format("'{}' must be an offset-aware TOML timestamp", field));
    const auto& stamp = value->get();
    const auto date = year(stamp.date.year) / month(stamp.date.month) / day(stamp.date.day);
    if (!date.ok() || stamp.time.second > 59 || stamp.time.minute > 59 || stamp.time.hour > 23
        || stamp.time.nanosecond >= 1'000'000'000)
        invalid(node, std::format("'{}' has an invalid date or time", field));
    const auto utc = sys_days(date) + hours(stamp.time.hour) + minutes(stamp.time.minute)
                   + seconds(stamp.time.second) - minutes(stamp.offset->minutes);
    const auto utc_year = int(year_month_day(floor<days>(utc)).year());
    if (utc_year < 1 || utc_year > 9999)
        invalid(node, std::format("'{}' is outside the supported UTC date range", field));
    return {utc, nanoseconds(stamp.time.nanosecond)};
}

template<class Map, class Key, class Value>
bool Loader::define(Map& map, Key key, Value value, const toml::node& node, std::string description)
{
    const auto location = at(node.source());
    const auto [it, added] = map.try_emplace(std::move(key), Defined<Value>{std::move(value), location});
    if (!added)
        report(DiagnosticKind::duplicate, location, "duplicate " + description, it->second.location);
    return added;
}

std::vector<std::string> Loader::ids(const toml::node& node, std::string_view field)
{
    const auto* values = node.as_array();
    if (!values) invalid(node, std::format("'{}' must be an array of identifiers", field));
    std::map<std::string, Defined<bool>, std::less<>> unique;
    for (const auto& value : *values) {
        auto name = id(value, field);
        define(unique, name, true, value, std::format("'{}' in '{}'", name, field));
    }
    return unique | std::views::keys | std::ranges::to<std::vector>();
}

template<class Read>
void Loader::records(const toml::table& table, std::string_view field, Read read)
{
    const auto* value = table.get(field);
    if (!value) return;
    const auto* array = value->as_array();
    if (!array) {
        report(DiagnosticKind::schema, at(value->source()),
               std::format("'{}' must be an array of tables", field));
        return;
    }
    for (const auto& item : *array) {
        try {
            const auto* record = item.as_table();
            if (!record) invalid(item, std::format("'{}' entries must be tables", field));
            read(*record);
        } catch (const InvalidRecord&) {
            // Keep diagnosing other records and files.
        }
    }
}

void Loader::read_header(const toml::node& node)
{
    const auto* table = node.as_table();
    if (!table) invalid(node, "'catalog' must be a table");
    if (header_) {
        report(DiagnosticKind::duplicate, at(node.source()), "duplicate [catalog] header", header_->location);
        return;
    }
    keys(*table, {"schema_version", "revision", "published_at", "author"});
    const auto& version = required(*table, "schema_version");
    const auto major = integer(version, "schema_version");
    if (major != 1) invalid(version, std::format("unsupported catalog schema version {}", major));
    header_ = Defined<CatalogHeader>{
        {major, text(required(*table, "revision"), "revision", 128),
         timestamp(*table, "published_at"), text(required(*table, "author"), "author", 512)},
        at(node.source())};
}

void Loader::read_suite(const toml::table& table)
{
    keys(table, {"id", "title", "packages"});
    const auto& key = required(table, "id");
    const auto name = id(key, "id", true);
    Suite suite{text(required(table, "title"), "title", 512), ids(required(table, "packages"), "packages")};
    define(suites_, name, std::move(suite), key, std::format("suite '{}'", name));
}

void Loader::read_package(const toml::table& table)
{
    keys(table, {"id", "title", "kind"});
    const auto& key = required(table, "id");
    const auto name = id(key, "id");
    Package package{text(required(table, "title"), "title", 512), id(required(table, "kind"), "kind")};
    define(packages_, name, std::move(package), key, std::format("package '{}'", name));
}

void Loader::read_build(const toml::table& table)
{
    keys(table, {"package", "id", "version", "published_at", "about_url", "visibility",
                 "requires", "artifact", "extension"});
    const auto& key = required(table, "id");
    const BuildKey build_key{id(required(table, "package"), "package"), id(key, "id")};
    Build build{text(required(table, "version"), "version", 128), timestamp(table, "published_at"),
                link(table, "about_url"), Visibility::public_build, {}, {}, {}};
    if (const auto* value = table.get("visibility")) {
        const auto flag = text(*value, "visibility", 16);
        if (flag == "targeted") build.visibility = Visibility::targeted;
        else if (flag != "public") invalid(*value, "'visibility' must be 'public' or 'targeted'");
    }
    if (const auto* value = table.get("requires")) build.requirements = ids(*value, "requires");

    std::map<std::string, Defined<Artifact>, std::less<>> artifacts;
    records(table, "artifact", [&](const toml::table& entry) {
        keys(entry, {"platform", "url", "media_type", "size", "sha256"});
        const auto& platform_field = required(entry, "platform");
        const auto platform = id(platform_field, "platform");
        const auto& media_field = required(entry, "media_type");
        auto media = text(media_field, "media_type", 128);
        if (!media_type(media)) invalid(media_field, "'media_type' must be a type/subtype");
        const auto& size_field = required(entry, "size");
        const auto size = integer(size_field, "size");
        if (size < 0) invalid(size_field, "'size' must be nonnegative");
        const auto& hash_field = required(entry, "sha256");
        const auto digest = parse_sha256(text(hash_field, "sha256", 64));
        if (!digest) invalid(hash_field, "'sha256' must have 64 hexadecimal digits");
        Artifact artifact{link(entry, "url"), std::move(media), static_cast<std::uint64_t>(size), *digest};
        define(artifacts, platform, std::move(artifact), platform_field, std::format("artifact platform '{}'", platform));
    });
    if (artifacts.empty()) report(DiagnosticKind::schema, at(table.source()), "build requires at least one artifact");
    build.artifacts = take_values(artifacts);

    // A global element overlaps every suite view. Scoped elements only
    // overlap other elements with the same namespace, name and suite.
    using ExtensionName = std::pair<std::string, std::string>;
    std::map<ExtensionName, std::map<std::optional<std::string>, SourceLocation>> scopes;
    std::vector<SourceLocation> extension_locations;
    records(table, "extension", [&](const toml::table& entry) {
        keys(entry, {"namespace", "name", "text", "suite"});
        const auto& namespace_field = required(entry, "namespace");
        const auto uri = text(namespace_field, "namespace", 2048);
        if (uri == core_namespace || uri == atom_namespace || uri == xml_namespace || uri == xmlns_namespace)
            invalid(namespace_field, "extension uses a reserved namespace");
        if (!valid_https_url(uri)) invalid(namespace_field, "'namespace' must be an absolute HTTPS URL");
        const auto& name_field = required(entry, "name");
        const auto name = text(name_field, "name", 128);
        if (!xml::valid_local_name(name)) invalid(name_field, "'name' must be an XML local name");
        Extension extension{uri, name, text(required(entry, "text"), "text", 4096, true), {}};
        const auto* suite = entry.get("suite");
        if (suite) extension.suite = id(*suite, "suite", true);
        auto& registered = scopes[{uri, name}];
        auto overlap = registered.begin();
        if (extension.suite) {
            overlap = registered.find(std::nullopt);
            if (overlap == registered.end()) overlap = registered.find(extension.suite);
        }
        if (overlap != registered.end()) {
            report(DiagnosticKind::duplicate, at(name_field.source()),
                   std::format("duplicate extension '{}' in an overlapping suite view", name), overlap->second);
            return;
        }
        registered.emplace(extension.suite, at(name_field.source()));
        extension_locations.push_back(at(suite ? suite->source() : name_field.source()));
        build.extensions.push_back(std::move(extension));
    });

    if (define(builds_, build_key, std::move(build), key, std::format("build '{}/{}'", build_key.package, build_key.id))
        && !extension_locations.empty())
        extension_locations_.emplace(build_key, std::move(extension_locations));
}

void Loader::read_track(const toml::table& table)
{
    keys(table, {"suite", "package", "name", "platform", "build", "installation_id"});
    const auto& name = required(table, "name");
    TrackKey key{id(required(table, "suite"), "suite", true), id(required(table, "package"), "package"),
                 id(name, "name"), id(required(table, "platform"), "platform"), {}};
    if (const auto* target = table.get("installation_id")) {
        key.installation_id = parse_uuid(text(*target, "installation_id", 36), false);
        if (!key.installation_id) invalid(*target, "'installation_id' must be a non-Nil UUIDv4");
    }
    auto label = std::format("track '{}/{}/{}/{}{}'", key.suite, key.package, key.name, key.platform,
                             key.installation_id ? " (targeted)" : "");
    define(tracks_, std::move(key), id(required(table, "build"), "build"), name, std::move(label));
}

void Loader::read_fragment(const toml::table& root)
{
    keys(root, {"catalog", "suite", "package", "build", "track"});
    if (const auto* header = root.get("catalog")) {
        try { read_header(*header); } catch (const InvalidRecord&) {}
    }
    records(root, "suite", [this](const auto& value) { read_suite(value); });
    records(root, "package", [this](const auto& value) { read_package(value); });
    records(root, "build", [this](const auto& value) { read_build(value); });
    records(root, "track", [this](const auto& value) { read_track(value); });
}

void Loader::validate_references()
{
    for (const auto& [id, suite] : suites_)
        for (const auto& package : suite.value.packages)
            if (!packages_.contains(package))
                report(DiagnosticKind::reference, suite.location,
                       std::format("suite '{}' references unknown package '{}'", id, package));

    for (const auto& [key, build] : builds_) {
        if (!packages_.contains(key.package))
            report(DiagnosticKind::reference, build.location,
                   std::format("build '{}' references unknown package '{}'", key.id, key.package));
        if (build.value.published_at > header_->value.published_at)
            report(DiagnosticKind::schema, build.location,
                   std::format("build '{}' is newer than the catalog publication time", key.id),
                   header_->location);
        for (std::size_t i = 0; i < build.value.extensions.size(); ++i) {
            const auto& extension = build.value.extensions[i];
            if (!extension.suite) continue;
            const auto suite = suites_.find(*extension.suite);
            if (suite == suites_.end() || !std::ranges::binary_search(suite->second.value.packages, key.package))
                report(DiagnosticKind::reference, extension_locations_.at(key)[i],
                       std::format("extension suite '{}' does not contain package '{}'", *extension.suite, key.package));
        }
    }

    for (const auto& [key, assignment] : tracks_) {
        const auto suite = suites_.find(key.suite);
        if (suite == suites_.end()) {
            report(DiagnosticKind::reference, assignment.location,
                   std::format("track references unknown suite '{}'", key.suite));
            continue;
        }
        if (!packages_.contains(key.package) || !std::ranges::binary_search(suite->second.value.packages, key.package)) {
            report(DiagnosticKind::reference, assignment.location,
                   std::format("track package '{}' is not defined in suite '{}'", key.package, key.suite));
            continue;
        }
        const auto build = builds_.find({key.package, assignment.value});
        if (build == builds_.end()) {
            report(DiagnosticKind::reference, assignment.location,
                   std::format("track references unknown build '{}/{}'", key.package, assignment.value));
            continue;
        }
        if (!build->second.value.artifacts.contains(key.platform))
            report(DiagnosticKind::reference, assignment.location,
                   std::format("track build has no artifact for platform '{}'", key.platform), build->second.location);
        if (!key.installation_id && build->second.value.visibility == Visibility::targeted)
            report(DiagnosticKind::reference, assignment.location, "public track cannot select a targeted-only build",
                   build->second.location);
    }
}

std::unique_ptr<const Catalog> Loader::finish(CatalogUsage usage, CatalogUsage limits)
{
    auto catalog = std::make_unique<Catalog>();
    catalog->usage = usage;
    catalog->limits = limits;
    catalog->header = std::move(header_->value);
    catalog->suites = take_values(suites_);
    catalog->packages = take_values(packages_);
    catalog->builds = take_values(builds_);
    catalog->tracks = take_values(tracks_);
    return catalog;
}

CatalogLoad input_error(DiagnosticKind kind, const fs::path& file, std::string message)
{
    return std::unexpected(std::vector<Diagnostic>{{kind, {file}, std::move(message), {}}});
}

CatalogLoad Loader::load(std::span<const fs::path> fragments, const CatalogUsage limits)
{
    if (limits.bytes < 0 || limits.fragments < 0)
        return input_error(DiagnosticKind::limit, {}, "catalog limits must be nonnegative");
    if (fragments.empty()) return input_error(DiagnosticKind::input, {}, "no catalog fragments supplied");
    if (std::ssize(fragments) > limits.fragments)
        return input_error(DiagnosticKind::limit, {}, "catalog exceeds the fragment count limit");

    auto remaining = limits.bytes;
    std::array<char, 16 * 1024> buffer{};
    for (const auto& file : fragments) {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            report(DiagnosticKind::input, {file}, ec ? ec.message() : "catalog fragment is not a regular file");
            continue;
        }
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            report(DiagnosticKind::input, {file}, "cannot open catalog fragment");
            continue;
        }
        std::string source;
        while (input.read(buffer.data(), std::ssize(buffer)) || input.gcount() != 0) {
            const auto count = input.gcount();
            if (count > remaining) {
                report(DiagnosticKind::limit, {file}, "catalog exceeds the combined byte limit");
                return std::unexpected(std::move(diagnostics_));
            }
            remaining -= count;
            source.append(buffer.data(), static_cast<std::size_t>(count));
        }
        if (input.bad() || !input.eof()) {
            report(DiagnosticKind::input, {file}, "cannot read catalog fragment");
            continue;
        }
        try {
            read_fragment(toml::parse(source, file.string()));
        } catch (const toml::parse_error& error) {
            report(DiagnosticKind::syntax, at(error.source()), std::string(error.description()));
        }
    }
    if (!diagnostics_.empty()) return std::unexpected(std::move(diagnostics_));
    if (!header_) return input_error(DiagnosticKind::schema, fragments.front(), "missing [catalog] header");
    validate_references();
    if (!diagnostics_.empty()) return std::unexpected(std::move(diagnostics_));
    return finish({.bytes = limits.bytes - remaining, .fragments = std::ssize(fragments)}, limits);
}

std::string location_text(const SourceLocation& location)
{
    auto file = location.file.empty() ? std::string("catalog") : location.file.string();
    if (location.line != 0) return std::format("{}:{}:{}", file, location.line, location.column);
    return file;
}

} // namespace

CatalogLoad load_catalog(std::span<const fs::path> fragments, CatalogUsage limits)
{
    return Loader{}.load(fragments, limits);
}

CatalogLoad load_catalog_directory(const fs::path& directory, const CatalogUsage limits)
{
    if (limits.bytes < 0 || limits.fragments < 0)
        return input_error(DiagnosticKind::limit, {}, "catalog limits must be nonnegative");
    std::error_code ec;
    const auto root = fs::canonical(directory, ec);
    if (ec) return input_error(DiagnosticKind::input, directory, "cannot resolve catalog directory: " + ec.message());

    std::vector<fs::path> files;
    const fs::directory_iterator end;
    for (fs::directory_iterator it(root, ec); !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".toml") continue;
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (!fs::is_regular_file(status))
            return input_error(DiagnosticKind::input, it->path(), "catalog directory entry must be a regular TOML file");
        if (std::ssize(files) == limits.fragments)
            return input_error(DiagnosticKind::limit, root, "catalog exceeds the fragment count limit");
        files.push_back(it->path());
    }
    if (ec) return input_error(DiagnosticKind::input, root, "cannot read catalog directory: " + ec.message());
    if (files.empty()) return input_error(DiagnosticKind::input, root, "no .toml fragments in catalog directory");
    std::ranges::sort(files);
    return load_catalog(files, limits);
}

std::string format_diagnostic(const Diagnostic& diagnostic)
{
    const auto location = location_text(diagnostic.location);
    if (diagnostic.related)
        return std::format("{}: {} (related: {})", location, diagnostic.message, location_text(*diagnostic.related));
    return std::format("{}: {}", location, diagnostic.message);
}

std::string format_success(const Catalog& catalog)
{
    const auto kibibytes = [](std::ptrdiff_t bytes) {
        return bytes / 1024 + (bytes % 1024 != 0);
    };
    const auto& usage = catalog.usage;
    return std::format("Success. {}/{} fragments, {}/{} kiBytes read.",
                       usage.fragments, catalog.limits.fragments,
                       kibibytes(usage.bytes), kibibytes(catalog.limits.bytes));
}

} // namespace updater
