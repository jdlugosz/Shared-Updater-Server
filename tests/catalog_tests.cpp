#include "updater/catalog.hpp"
#include "updater/uri.hpp"

#include <catch2/catch_test_macros.hpp>

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <algorithm>
#include <format>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#endif

namespace {

using namespace updater;
namespace fs = std::filesystem;

constexpr std::string_view header = R"(
[catalog]
schema_version = 1
revision = "sample-001"
published_at = 2026-11-04T18:00:00.500Z
author = "Długosz"
)";

constexpr std::string_view suite = R"(
[[suite]]
id = "launcher"
title = "Launcher & tools"
packages = ["launcher-app"]
)";

constexpr std::string_view package = R"(
[[package]]
id = "launcher-app"
title = "Launcher"
kind = "installer"
)";

constexpr std::string_view build = R"(
[[build]]
package = "launcher-app"
id = "build-001"
version = "1.2.3"
published_at = 2026-11-04T12:00:00.125-06:00
about_url = "https://example.invalid/launcher#notes"
requires = ["z-feature", "a-feature"]

[[build.artifact]]
platform = "windows-x64"
url = "https://example.invalid/downloads/Launcher%20x64.exe?channel=current"
media_type = "application/octet-stream"
size = 1234
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789ABCDEF"

[[build.extension]]
namespace = "https://example.invalid/updater/launcher"
name = "Łącze"
text = "<notes> & details"
suite = "launcher"
)";

constexpr std::string_view track = R"(
[[track]]
suite = "launcher"
package = "launcher-app"
name = "current"
platform = "windows-x64"
build = "build-001"
)";

constexpr auto targeted_id = "12345678-1234-4234-8234-1234567890AB";
constexpr Uuid targeted_bytes{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}, std::byte{0x12}, std::byte{0x34}, std::byte{0x42}, std::byte{0x34},
                              std::byte{0x82}, std::byte{0x34}, std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}, std::byte{0x90}, std::byte{0xab}};

class Fixture {
public:
    Fixture()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned n = 0; n < 100; ++n) {
            auto candidate = fs::temp_directory_path() /
                ("updater-catalog-test-" + std::to_string(stamp) + "-" + std::to_string(n));
            if (fs::create_directory(candidate)) {
                root = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("cannot reserve a catalog test directory");
    }

    ~Fixture()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    fs::path write(std::string_view name, std::string_view contents) const
    {
        const auto file = root / name;
        std::ofstream output(file, std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        return file;
    }

    void valid() const
    {
        write("00-catalog.toml", header);
        write("10-suite.toml", suite);
        write("20-package.toml", package);
        write("30-build.toml", build);
        write("40-track.toml", track);
    }

    CatalogLoad load(CatalogUsage limits = catalog_limits) const { return load_catalog_directory(root, limits); }

    fs::path root;
};

std::string replaced(std::string_view source, std::string_view from, std::string_view to)
{
    auto result = std::string(source);
    const auto at = result.find(from);
    REQUIRE(at != std::string::npos);
    result.replace(at, from.size(), to);
    return result;
}

void require_error(const CatalogLoad& result, DiagnosticKind kind, std::string_view message)
{
    REQUIRE_FALSE(result.has_value());
    INFO([&] {
        std::string combined;
        for (const auto& item : result.error()) combined += format_diagnostic(item) + "\n";
        return combined;
    }());
    REQUIRE(std::ranges::any_of(result.error(), [&](const auto& item) {
        return item.kind == kind && item.message.find(message) != std::string::npos;
    }));
}

} // namespace

TEST_CASE("Catalog merges separate definitions and owns the resulting data", "[catalog]")
{
    std::unique_ptr<const Catalog> catalog;
    {
        Fixture fixture;
        fixture.valid();
        fixture.write("README.txt", "ignored: this is not TOML");
        auto result = fixture.load();
        if (!result) FAIL(format_diagnostic(result.error().front()));
        catalog = std::move(*result);
    }
    REQUIRE(catalog->header.author == "Długosz");
    REQUIRE(catalog->header.revision == "sample-001");
    REQUIRE(catalog->suites.at("launcher").title == "Launcher & tools");
    REQUIRE(catalog->suites.at("launcher").packages == std::vector<std::string>{"launcher-app"});
    REQUIRE(catalog->packages.at("launcher-app").kind == "installer");
    const auto& record = catalog->builds.at({"launcher-app", "build-001"});
    REQUIRE(record.version == "1.2.3");
    REQUIRE(record.visibility == Visibility::public_build);
    REQUIRE(record.requirements == std::vector<std::string>{"a-feature", "z-feature"});
    REQUIRE(record.published_at.seconds == catalog->header.published_at.seconds);
    REQUIRE(record.published_at.fraction == std::chrono::milliseconds(125));
    const auto& artifact = record.artifacts.at("windows-x64");
    REQUIRE(artifact.size == 1234);
    REQUIRE(artifact.sha256.front() == std::byte{0x01});
    REQUIRE(artifact.sha256.back() == std::byte{0xef});
    REQUIRE(artifact.url.ends_with("Launcher%20x64.exe?channel=current"));
    REQUIRE(record.extensions.at(0).name == "Łącze");
    REQUIRE(record.extensions.at(0).text == "<notes> & details");
    REQUIRE(record.extensions.at(0).suite == "launcher");
    REQUIRE(catalog->tracks.at({"launcher", "launcher-app", "current", "windows-x64", {}}) == "build-001");
}

TEST_CASE("Explicit file ordering does not affect resolution", "[catalog]")
{
    Fixture fixture;
    const auto files = std::array{
        fixture.write("track.toml", track), fixture.write("build.toml", build),
        fixture.write("suite.toml", suite), fixture.write("package.toml", package),
        fixture.write("catalog.toml", header)
    };
    auto reversed = files;
    std::ranges::reverse(reversed);
    const auto first = load_catalog(files);
    const auto second = load_catalog(reversed);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE((*first)->tracks == (*second)->tracks);
    REQUIRE((*first)->builds.at({"launcher-app", "build-001"}).version
        == (*second)->builds.at({"launcher-app", "build-001"}).version);
}

TEST_CASE("Catalog keeps keys paired with their values after gathering unordered records", "[catalog]")
{
    Fixture fixture;
    std::vector<fs::path> files;
    for (const auto label : {"zulu", "mike", "alpha"}) {
        files.push_back(fixture.write(std::string(label) + ".toml", std::format(R"(
[[suite]]
id = "suite-{0}"
title = "Suite {0}"
packages = ["package-{0}"]

[[package]]
id = "package-{0}"
title = "Package {0}"
kind = "installer"

[[build]]
package = "package-{0}"
id = "build-{0}"
version = "{0}"
published_at = 2026-11-04T12:00:00Z
about_url = "https://example.invalid/{0}"

[[build.artifact]]
platform = "windows-x64"
url = "https://example.invalid/{0}/windows"
media_type = "application/octet-stream"
size = 11
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

[[build.artifact]]
platform = "linux-x64"
url = "https://example.invalid/{0}/linux"
media_type = "application/octet-stream"
size = 22
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

[[track]]
suite = "suite-{0}"
package = "package-{0}"
name = "current"
platform = "windows-x64"
build = "build-{0}"
)", label)));
    }
    files.push_back(fixture.write("catalog.toml", header));
    const auto loaded = load_catalog(files);
    REQUIRE(loaded.has_value());
    const auto& catalog = **loaded;
    std::vector<std::string> suite_ids;
    for (const auto& [id, unused] : catalog.suites) suite_ids.push_back(id);
    REQUIRE(suite_ids == std::vector<std::string>{"suite-alpha", "suite-mike", "suite-zulu"});
    for (const std::string label : {"alpha", "mike", "zulu"}) {
        const auto suite_id = "suite-" + label;
        const auto package_id = "package-" + label;
        const auto build_id = "build-" + label;
        const auto selected_suite = catalog.suites.find(std::string_view(suite_id));
        REQUIRE(selected_suite != catalog.suites.end());
        REQUIRE(selected_suite->second.title == "Suite " + label);
        REQUIRE(selected_suite->second.packages == std::vector<std::string>{package_id});
        REQUIRE(catalog.packages.at(package_id).title == "Package " + label);
        const auto& selected_build = catalog.builds.at({package_id, build_id});
        REQUIRE(selected_build.version == label);
        REQUIRE(selected_build.artifacts.begin()->first == "linux-x64");
        REQUIRE(selected_build.artifacts.at("linux-x64").url == "https://example.invalid/" + label + "/linux");
        REQUIRE(selected_build.artifacts.at("linux-x64").size == 22);
        REQUIRE(selected_build.artifacts.at("windows-x64").url == "https://example.invalid/" + label + "/windows");
        REQUIRE(selected_build.artifacts.at("windows-x64").size == 11);
        REQUIRE(catalog.tracks.at({suite_id, package_id, "current", "windows-x64", {}}) == build_id);
    }
}

TEST_CASE("Successful loads report their fragment and byte usage", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    fixture.write("README.txt", "ignored input");
    const auto loaded = fixture.load();
    REQUIRE(loaded.has_value());
    const auto& usage = (*loaded)->usage;
    REQUIRE(usage.fragments == 5);
    REQUIRE(usage.bytes == std::ssize(header) + std::ssize(suite) + std::ssize(package)
        + std::ssize(build) + std::ssize(track));
    REQUIRE((*loaded)->limits.fragments == catalog_limits.fragments);
    REQUIRE((*loaded)->limits.bytes == catalog_limits.bytes);

    const auto files = std::array{fixture.root / "00-catalog.toml"};
    const auto explicit_load = load_catalog(files, {.bytes = 1024, .fragments = 10});
    REQUIRE(explicit_load.has_value());
    REQUIRE((*explicit_load)->usage.bytes == std::ssize(header));
    REQUIRE((*explicit_load)->limits.bytes == 1024);
    REQUIRE((*explicit_load)->limits.fragments == 10);
    REQUIRE(format_success(**explicit_load) == "Success. 1/10 fragments, 1/1 kiBytes read.");
}

TEST_CASE("Success report matches the requested example for a real catalog", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    for (unsigned i = 0; i < 48; ++i)
        fixture.write("extra-" + std::to_string(i) + ".toml", "#\n");
    constexpr std::ptrdiff_t total = 81 * 1024;
    const auto used = std::ssize(header) + std::ssize(suite) + std::ssize(package)
        + std::ssize(build) + std::ssize(track) + 48 * 2;
    const auto padding = total - used;
    fixture.write("00-catalog.toml", std::string(header) + "\n#"
        + std::string(static_cast<std::size_t>(padding - 3), ' ') + "\n");
    const auto loaded = fixture.load();
    REQUIRE(loaded.has_value());
    REQUIRE((*loaded)->usage.bytes == total);
    REQUIRE(format_success(**loaded) == "Success. 53/4096 fragments, 81/8192 kiBytes read.");
}

TEST_CASE("Success reports round partial kiBytes upward", "[catalog]")
{
    Fixture fixture;
    std::ptrdiff_t total = 0;
    std::string expected;
    SECTION("exact kiByte") {
        total = 1024;
        expected = "Success. 1/4 fragments, 1/1 kiBytes read.";
    }
    SECTION("partial kiByte") {
        total = 1025;
        expected = "Success. 1/4 fragments, 2/2 kiBytes read.";
    }
    fixture.write("catalog.toml", std::string(header) + "\n#"
        + std::string(static_cast<std::size_t>(total - std::ssize(header) - 3), ' ') + "\n");
    const auto loaded = fixture.load({.bytes = total, .fragments = 4});
    REQUIRE(loaded.has_value());
    REQUIRE((*loaded)->usage.bytes == total);
    REQUIRE(format_success(**loaded) == expected);
}

TEST_CASE("Success reporting handles zero and maximum signed byte counts", "[catalog]")
{
    Catalog catalog;
    REQUIRE(format_success(catalog) == "Success. 0/4096 fragments, 0/8192 kiBytes read.");
    const auto maximum = std::numeric_limits<std::ptrdiff_t>::max();
    catalog.usage = {.bytes = maximum, .fragments = 1};
    catalog.limits = catalog.usage;
    const auto rounded = std::to_string(maximum / 1024 + 1);
    REQUIRE(format_success(catalog) == "Success. 1/1 fragments, " + rounded + "/" + rounded + " kiBytes read.");
}

TEST_CASE("A directory selector can point at a deployment slot", "[catalog]")
{
    Fixture fixture;
    fs::create_directory(fixture.root / "slot");
    fixture.write("slot/catalog.toml", header);
    fs::create_directory_symlink(fixture.root / "slot", fixture.root / "current");
    const auto loaded = load_catalog_directory(fixture.root / "current");
    REQUIRE(loaded.has_value());
    REQUIRE((*loaded)->header.revision == "sample-001");
    REQUIRE((*loaded)->suites.empty());
}

TEST_CASE("A suite can publish an empty catalog", "[catalog]")
{
    Fixture fixture;
    fixture.write("catalog.toml", header);
    fixture.write("suite.toml", replaced(suite, "[\"launcher-app\"]", "[]"));
    const auto loaded = fixture.load();
    REQUIRE(loaded.has_value());
    REQUIRE((*loaded)->suites.at("launcher").packages.empty());
}

TEST_CASE("Suites can share a package with multiple platform artifacts", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    fixture.write("11-other.toml", replaced(suite, "id = \"launcher\"", "id = 'other'"));
    const auto artifact_start = build.find("[[build.artifact]]");
    const auto artifact_end = build.find("[[build.extension]]");
    const auto arm = replaced(build.substr(artifact_start, artifact_end - artifact_start),
                              "windows-x64", "windows-arm64");
    fixture.write("30-build.toml", std::string(build) + arm);
    auto arm_track = replaced(track, "suite = \"launcher\"", "suite = 'other'");
    arm_track = replaced(arm_track, "windows-x64", "windows-arm64");
    fixture.write("41-track.toml", arm_track);
    const auto loaded = fixture.load();
    REQUIRE(loaded.has_value());
    REQUIRE((*loaded)->suites.at("other").packages == (*loaded)->suites.at("launcher").packages);
    REQUIRE((*loaded)->builds.at({"launcher-app", "build-001"}).artifacts.size() == 2);
    REQUIRE((*loaded)->tracks.at({"other", "launcher-app", "current", "windows-arm64", {}}) == "build-001");
}

TEST_CASE("Targeted assignments coexist with public tracks", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    auto targeted_build = replaced(build, "id = \"build-001\"", "id = \"build-002\"\nvisibility = \"targeted\"");
    fixture.write("31-target-build.toml", targeted_build);
    auto targeted_track = replaced(track, "build = \"build-001\"", "build = \"build-002\"");
    targeted_track += std::string("installation_id = \"") + targeted_id + "\"\n";
    fixture.write("41-target-track.toml", targeted_track);
    const auto loaded = fixture.load();
    if (!loaded) FAIL(format_diagnostic(loaded.error().front()));
    const auto& catalog = **loaded;
    REQUIRE(catalog.tracks.size() == 2);
    REQUIRE(catalog.tracks.at({"launcher", "launcher-app", "current", "windows-x64", {}}) == "build-001");
    REQUIRE(catalog.tracks.at({"launcher", "launcher-app", "current", "windows-x64", targeted_bytes}) == "build-002");
    REQUIRE(catalog.builds.at({"launcher-app", "build-002"}).visibility == Visibility::targeted);
}

TEST_CASE("Duplicate definitions report both source locations", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    const auto second = fixture.write("99-duplicate.toml", suite);
    const auto result = fixture.load();
    require_error(result, DiagnosticKind::duplicate, "duplicate suite 'launcher'");
    const auto& error = result.error().front();
    REQUIRE(error.location.file == second);
    REQUIRE(error.location.line == 3);
    REQUIRE(error.location.column != 0);
    REQUIRE(error.related.has_value());
    REQUIRE(error.related->file.filename() == "10-suite.toml");
    REQUIRE(error.related->line == 3);
    REQUIRE(format_diagnostic(error).find("99-duplicate.toml:3:") != std::string::npos);
    REQUIRE(format_diagnostic(error).find("10-suite.toml:3:") != std::string::npos);
}

TEST_CASE("Duplicate identities in every catalog collection are rejected", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    std::string_view duplicate;
    std::string_view message;
    SECTION("catalog header") { duplicate = header; message = "[catalog] header"; }
    SECTION("package") { duplicate = package; message = "package 'launcher-app'"; }
    SECTION("build") { duplicate = build; message = "build 'launcher-app/build-001'"; }
    SECTION("track") { duplicate = track; message = "track 'launcher/launcher-app/current/windows-x64'"; }
    fixture.write("99-duplicate.toml", duplicate);
    require_error(fixture.load(), DiagnosticKind::duplicate, message);
}

TEST_CASE("Duplicates in arrays and nested artifact records are rejected", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    SECTION("package membership") {
        fixture.write("10-suite.toml", replaced(suite, "[\"launcher-app\"]", "[\"launcher-app\", \"launcher-app\"]"));
        require_error(fixture.load(), DiagnosticKind::duplicate, "in 'packages'");
    }
    SECTION("required capabilities") {
        fixture.write("30-build.toml", replaced(build, "\"z-feature\"", "\"a-feature\""));
        require_error(fixture.load(), DiagnosticKind::duplicate, "in 'requires'");
    }
    SECTION("platform artifact") {
        const auto artifact = build.substr(build.find("[[build.artifact]]"),
            build.find("[[build.extension]]") - build.find("[[build.artifact]]"));
        fixture.write("30-build.toml", std::string(build) + std::string(artifact));
        require_error(fixture.load(), DiagnosticKind::duplicate, "artifact platform");
    }
}

TEST_CASE("Parser errors and field errors from different files are retained", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    fixture.write("50-broken.toml", "[[package]\n");
    fixture.write("51-typo.toml", "[[package]]\nid = 'unused'\ntitel = 'Typo'\nkind = 'source'\n");
    const auto result = fixture.load();
    require_error(result, DiagnosticKind::syntax, "");
    require_error(result, DiagnosticKind::schema, "unknown field 'titel'");
    require_error(result, DiagnosticKind::schema, "missing required field 'title'");
    const auto parse = std::ranges::find(result.error(), DiagnosticKind::syntax, &Diagnostic::kind);
    REQUIRE(parse != result.error().end());
    REQUIRE(parse->location.file.filename() == "50-broken.toml");
    REQUIRE(parse->location.line != 0);
}

TEST_CASE("Schema rejects unsupported headers and wrong TOML types", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    SECTION("missing catalog metadata") {
        fixture.write("00-catalog.toml", "# empty");
        require_error(fixture.load(), DiagnosticKind::schema, "missing [catalog] header");
    }
    SECTION("unsupported version") {
        fixture.write("00-catalog.toml", replaced(header, "schema_version = 1", "schema_version = 2"));
        require_error(fixture.load(), DiagnosticKind::schema, "unsupported catalog schema version");
    }
    SECTION("catalog table required") {
        fixture.write("00-catalog.toml", "catalog = true");
        require_error(fixture.load(), DiagnosticKind::schema, "must be a table");
    }
    SECTION("array of tables required") {
        fixture.write("10-suite.toml", "suite = [1]");
        require_error(fixture.load(), DiagnosticKind::schema, "entries must be tables");
    }
    SECTION("integer size required") {
        fixture.write("30-build.toml", replaced(build, "size = 1234", "size = 1234.0"));
        require_error(fixture.load(), DiagnosticKind::schema, "size' must be an integer");
    }
    SECTION("canonical lowercase suite") {
        fixture.write("10-suite.toml", replaced(suite, "\"launcher\"", "\"Launcher\""));
        require_error(fixture.load(), DiagnosticKind::schema, "canonical lowercase identifier");
    }
}

TEST_CASE("Catalog validates cross-fragment references and audiences", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    SECTION("suite membership") {
        fixture.write("10-suite.toml", replaced(suite, "\"launcher-app\"", "\"missing\""));
        require_error(fixture.load(), DiagnosticKind::reference, "references unknown package 'missing'");
    }
    SECTION("build package") {
        fixture.write("30-build.toml", replaced(build, "package = \"launcher-app\"", "package = \"missing\""));
        require_error(fixture.load(), DiagnosticKind::reference, "references unknown package 'missing'");
    }
    SECTION("track suite") {
        fixture.write("40-track.toml", replaced(track, "\"launcher\"", "\"missing\""));
        require_error(fixture.load(), DiagnosticKind::reference, "track references unknown suite");
    }
    SECTION("track package outside the suite") {
        fixture.write("10-suite.toml", replaced(suite, "[\"launcher-app\"]", "[]"));
        require_error(fixture.load(), DiagnosticKind::reference, "is not defined in suite");
    }
    SECTION("track build") {
        fixture.write("40-track.toml", replaced(track, "\"build-001\"", "\"unknown\""));
        require_error(fixture.load(), DiagnosticKind::reference, "track references unknown build");
    }
    SECTION("track platform") {
        fixture.write("40-track.toml", replaced(track, "\"windows-x64\"", "\"windows-arm64\""));
        require_error(fixture.load(), DiagnosticKind::reference, "has no artifact for platform");
    }
    SECTION("public track selecting a targeted build") {
        fixture.write("30-build.toml", replaced(build, "version = \"1.2.3\"", "version = \"1.2.3\"\nvisibility = 'targeted'"));
        require_error(fixture.load(), DiagnosticKind::reference, "public track cannot select a targeted-only build");
    }
    SECTION("extension suite") {
        fixture.write("30-build.toml", replaced(build, "suite = \"launcher\"", "suite = 'other'"));
        require_error(fixture.load(), DiagnosticKind::reference, "extension suite 'other'");
    }
}

TEST_CASE("Offset-aware dates preserve fractional ordering", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    SECTION("build published later in the same second") {
        fixture.write("30-build.toml", replaced(build, ".125-06:00", ".501-06:00"));
        require_error(fixture.load(), DiagnosticKind::schema, "newer than the catalog publication time");
    }
    SECTION("local date-time without an offset") {
        fixture.write("30-build.toml", replaced(build, ".125-06:00", ".125"));
        require_error(fixture.load(), DiagnosticKind::schema, "offset-aware TOML timestamp");
    }
    SECTION("date normalized outside the representable year range") {
        fixture.write("30-build.toml", replaced(build, "2026-11-04T12:00:00.125-06:00", "0001-01-01T00:00:00+01:00"));
        require_error(fixture.load(), DiagnosticKind::schema, "supported UTC date range");
    }
}

TEST_CASE("Artifacts must have validated transport metadata", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    SECTION("missing artifacts") {
        fixture.write("30-build.toml", build.substr(0, build.find("[[build.artifact]]")));
        require_error(fixture.load(), DiagnosticKind::schema, "requires at least one artifact");
    }
    SECTION("negative size") {
        fixture.write("30-build.toml", replaced(build, "size = 1234", "size = -1"));
        require_error(fixture.load(), DiagnosticKind::schema, "size' must be nonnegative");
    }
    SECTION("invalid digest") {
        fixture.write("30-build.toml", replaced(build, "0123456789ABCDEF", "0123456789ABCDEZ"));
        require_error(fixture.load(), DiagnosticKind::schema, "64 hexadecimal digits");
    }
    SECTION("invalid media type") {
        fixture.write("30-build.toml", replaced(build, "application/octet-stream", "application"));
        require_error(fixture.load(), DiagnosticKind::schema, "must be a type/subtype");
    }
}

TEST_CASE("HTTPS fields accept valid authorities and reject invalid URLs", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    constexpr auto original = "https://example.invalid/launcher#notes";
    SECTION("IP literal, port and escaped query") {
        fixture.write("30-build.toml", replaced(build, original, "HTTPS://[2001:db8::1]:8443/a?q=%26#notes"));
        REQUIRE(fixture.load().has_value());
    }
    SECTION("embedded NUL inside an IPv6 literal") {
        using namespace std::string_view_literals;
        REQUIRE_FALSE(valid_https_url("https://[::1\0ignored]/"sv));
    }
    SECTION("invalid URLs") {
        for (const auto* url : {"/relative", "http://example.invalid", "https:///path",
                                "https://user@example.invalid", "https://bad..example.invalid",
                                "https://example.invalid:65536", "https://[:::]/", "https://example.invalid/%2",
                                "https://example.invalid/x y", "https://example.invalid/#one#two"}) {
            CAPTURE(url);
            fixture.write("30-build.toml", replaced(build, original, url));
            require_error(fixture.load(), DiagnosticKind::schema, "absolute HTTPS URL");
        }
    }
}

TEST_CASE("Extension data must be valid XML and must not collide", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    SECTION("XML control characters in TOML strings") {
        fixture.write("30-build.toml", replaced(build, "<notes> & details", R"(\u0001)"));
        require_error(fixture.load(), DiagnosticKind::schema, "XML 1.0 cannot represent");
    }
    SECTION("namespace prefix in a local name") {
        fixture.write("30-build.toml", replaced(build, "Łącze", "upd:note"));
        require_error(fixture.load(), DiagnosticKind::schema, "XML local name");
    }
    SECTION("reserved namespace") {
        fixture.write("30-build.toml", replaced(build, "https://example.invalid/updater/launcher",
                                                 "https://dlugosz.enterprises/ns/updater/1"));
        require_error(fixture.load(), DiagnosticKind::schema, "reserved namespace");
    }
    SECTION("global and scoped extensions overlap") {
        auto entry = std::string(build.substr(build.find("[[build.extension]]")));
        entry.erase(entry.find("suite = "));
        fixture.write("30-build.toml", std::string(build) + entry);
        require_error(fixture.load(), DiagnosticKind::duplicate, "overlapping suite view");
    }
    SECTION("separate suite views can reuse a name") {
        fixture.write("11-other.toml", replaced(suite, "id = \"launcher\"", "id = 'other'"));
        auto entry = replaced(build.substr(build.find("[[build.extension]]")), "suite = \"launcher\"", "suite = 'other'");
        fixture.write("30-build.toml", std::string(build) + entry);
        REQUIRE(fixture.load().has_value());
    }
}

TEST_CASE("Target UUIDs are version 4 and compare by bytes", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    SECTION("invalid target IDs") {
        for (const auto* id : {"00000000-0000-0000-0000-000000000000", "12345678-1234-1234-8234-1234567890ab",
                               "12345678-1234-4234-7234-1234567890ab", "12345678-1234-4234-8234-1234567890a-",
                               "123456781234423482341234567890ab"}) {
            CAPTURE(id);
            fixture.write("40-track.toml", std::string(track) + "installation_id = '" + id + "'\n");
            require_error(fixture.load(), DiagnosticKind::schema, "non-Nil UUIDv4");
        }
    }
    SECTION("same ID in different hexadecimal case") {
        fixture.write("41-track.toml", std::string(track) + "installation_id = '12345678-1234-4234-8234-1234567890ab'\n");
        fixture.write("42-track.toml", std::string(track) + "installation_id = '" + targeted_id + "'\n");
        require_error(fixture.load(), DiagnosticKind::duplicate, "targeted");
    }
}

TEST_CASE("Gathering reports missing input, invalid entries and resource limits", "[catalog]")
{
    Fixture fixture;
    SECTION("empty directory") {
        require_error(fixture.load(), DiagnosticKind::input, "no .toml fragments");
    }
    SECTION("missing directory") {
        require_error(load_catalog_directory(fixture.root / "missing"), DiagnosticKind::input, "cannot resolve");
    }
    SECTION("missing explicit file") {
        const auto files = std::array{fixture.root / "missing.toml"};
        const auto result = load_catalog(files);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().front().kind == DiagnosticKind::input);
        REQUIRE(result.error().front().location.file == files.front());
    }
    SECTION("symbolic link as a fragment") {
        const auto target = fixture.write("catalog.txt", header);
        fs::create_symlink(target, fixture.root / "catalog.toml");
        require_error(fixture.load(), DiagnosticKind::input, "regular TOML file");
    }
    SECTION("combined byte budget includes every fragment") {
        fixture.write("catalog.toml", header);
        const auto extra = "#" + std::string(20000, ' ') + "\n";
        fixture.write("second.toml", extra);
        const auto bytes = std::ssize(header) + std::ssize(extra);
        REQUIRE(fixture.load({.bytes = bytes, .fragments = 2}).has_value());
        require_error(fixture.load({.bytes = bytes - 1, .fragments = 2}),
                      DiagnosticKind::limit, "combined byte limit");
    }
    SECTION("fragment count budget") {
        fixture.valid();
        require_error(fixture.load({.bytes = catalog_limits.bytes, .fragments = 4}),
                      DiagnosticKind::limit, "fragment count limit");
        const auto files = std::array{fixture.root / "00-catalog.toml", fixture.root / "10-suite.toml"};
        require_error(load_catalog(files, {.bytes = catalog_limits.bytes, .fragments = 1}),
                      DiagnosticKind::limit, "fragment count limit");
    }
}

TEST_CASE("Signed catalog budgets reject negative limits and enforce zero ceilings", "[catalog]")
{
    Fixture fixture;
    const auto files = std::array{fixture.write("catalog.toml", header)};
    auto limits = catalog_limits;
    std::string_view message;
    SECTION("negative byte limit") {
        limits.bytes = -1;
        message = "limits must be nonnegative";
    }
    SECTION("negative fragment limit") {
        limits.fragments = -1;
        message = "limits must be nonnegative";
    }
    SECTION("zero byte limit") {
        limits.bytes = 0;
        message = "combined byte limit";
    }
    SECTION("zero fragment limit") {
        limits.fragments = 0;
        message = "fragment count limit";
    }
    require_error(load_catalog(files, limits), DiagnosticKind::limit, message);
    require_error(fixture.load(limits), DiagnosticKind::limit, message);
}

TEST_CASE("A malformed catalog has bounded diagnostics", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    std::string extra;
    for (unsigned n = 0; n < 150; ++n)
        extra += "unknown_" + std::to_string(n) + " = 1\n";
    fixture.write("50-unknown.toml", extra);
    const auto loaded = fixture.load();
    require_error(loaded, DiagnosticKind::limit, "further catalog diagnostics omitted");
    REQUIRE(loaded.error().size() == 101);
    REQUIRE(loaded.error().back().location.file.filename() == "50-unknown.toml");
}

TEST_CASE("A rejected replacement cannot mutate an already loaded catalog", "[catalog]")
{
    Fixture fixture;
    fixture.valid();
    const auto current = fixture.load();
    REQUIRE(current.has_value());
    fixture.write("00-catalog.toml", replaced(header, "sample-001", "rejected"));
    fixture.write("40-track.toml", replaced(track, "\"build-001\"", "\"missing\""));
    const auto rejected = fixture.load();
    require_error(rejected, DiagnosticKind::reference, "unknown build");
    REQUIRE((*current)->header.revision == "sample-001");
    REQUIRE((*current)->tracks.at({"launcher", "launcher-app", "current", "windows-x64", {}}) == "build-001");
}
