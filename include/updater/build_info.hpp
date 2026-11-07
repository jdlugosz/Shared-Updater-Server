// Shared Updater Server - include/updater/build_info.hpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#pragma once

#include <string_view>

namespace updater {

struct BuildInfo {
    std::string_view version;
    std::string_view status;
    std::string_view configuration;
    std::string_view variant;
    std::string_view date;
    std::string_view git_commit;
    std::string_view git_tree;
    std::string_view git_tag;
    std::string_view report;

    [[nodiscard]] constexpr std::string_view describe() const noexcept { return report; }
};

extern const BuildInfo build_info; // The build generates this definition.

} // namespace updater
