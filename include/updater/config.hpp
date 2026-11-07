// Shared Updater Server - include/updater/config.hpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#pragma once

#include "updater/http_server.hpp"

namespace updater {

struct ServerConfig {
    HttpOptions http{};
    WorkerOptions worker{};
    FeedOptions feed{};
    CatalogUsage catalog_budget = catalog_limits;
    std::filesystem::path catalog_directory;
    std::vector<std::filesystem::path> catalog_files;
    std::filesystem::path state_directory = "/var/lib/shared-updater-server";
    std::filesystem::path log_directory = "/var/log/shared-updater-server";
    std::ptrdiff_t log_bytes = 1024 * 1024;
    std::chrono::milliseconds sqlite_busy = std::chrono::seconds(2);
    std::chrono::milliseconds worker_budget = std::chrono::seconds(10);
    std::chrono::milliseconds reload_timeout = std::chrono::seconds(30);
    std::chrono::milliseconds shutdown_timeout = std::chrono::seconds(20);
};

[[nodiscard]] Result<ServerConfig> read_server_config(const std::filesystem::path& file);
[[nodiscard]] Result<std::unique_ptr<const Catalog>> prepare_catalog(const ServerConfig& config);

} // namespace updater
