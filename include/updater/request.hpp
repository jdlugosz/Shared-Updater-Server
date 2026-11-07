// Shared Updater Server - include/updater/request.hpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#pragma once

#include "updater/binary.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace updater {

using WallTime = std::chrono::sys_seconds;
using MonoTime = std::chrono::steady_clock::time_point;

struct Clocks {
    std::function<WallTime()> wall = [] {
        return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    };
    std::function<MonoTime()> monotonic = [] { return std::chrono::steady_clock::now(); };
};

struct CheckRequest {
    Uuid installation_id{};
    std::string suite;
    std::vector<std::string> platforms{}; // Empty requests all platforms.
};

struct RequestContext {
    std::int64_t sequence = 0;
    WallTime observed_at{};
    MonoTime admitted_at{};
    MonoTime queue_deadline{};
};

enum class RouteError { invalid, not_found };
[[nodiscard]] std::expected<CheckRequest, RouteError> parse_check_target(std::string_view target);

enum class Outcome { ok, not_found, unavailable };
struct Reply {
    Outcome outcome = Outcome::unavailable;
    std::string body;
    std::string category;
    std::string detail{};
};

} // namespace updater
