// Shared Updater Server - tests/support.hpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#pragma once

#include "updater/catalog.hpp"

#include <atomic>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace test {
using namespace std::chrono_literals;

inline updater::Timestamp published()
{
    return {std::chrono::sys_days(std::chrono::year(2026)/11/5) + 12h, 500ms};
}

inline std::unique_ptr<updater::Catalog> catalog(std::string revision = "sample-001")
{
    using namespace updater;
    auto result = std::make_unique<Catalog>();
    result->header = {1, std::move(revision), published(), "Długosz"};
    result->suites.emplace("launcher", Suite{"Launcher & tools", {"app"}});
    result->suites.emplace("other", Suite{"Other suite", {"app"}});
    result->packages.emplace("app", Package{"Launcher", "installer"});
    Build build;
    build.version = "1.2.3";
    build.published_at = published();
    build.published_at.fraction = 125ms;
    build.about_url = "https://example.invalid/notes?a=1&b=2";
    build.requirements = {"signed-packages"};
    build.artifacts.emplace("windows-x64", Artifact{
        "https://example.invalid/Launcher%20x64.exe?a=1&b=2", "application/octet-stream", 1234,
        *parse_sha256("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")});
    build.extensions.push_back({"https://example.invalid/extensions", "Łącze", "<notes> & details", "launcher"});
    result->builds.emplace(BuildKey{"app", "build-001"}, std::move(build));
    result->tracks.emplace(TrackKey{"launcher", "app", "current", "windows-x64", {}}, "build-001");
    return result;
}

class Directory {
public:
    Directory()
    {
        static std::atomic<unsigned> sequence{};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path()
            / std::format("updater-test-{}-{}", stamp, sequence++);
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("test directory exists");
    }

    ~Directory() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;

    std::filesystem::path write(std::string_view name, std::string_view content) const
    {
        const auto path = root / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.close();
        return path;
    }
    std::filesystem::path root;
};
} // namespace test
