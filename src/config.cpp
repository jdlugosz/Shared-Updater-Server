#include "updater/config.hpp"

#include <toml++/toml.hpp>
#include <boost/asio/ip/address.hpp>

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <charconv>
#include <algorithm>
#include <format>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string_view>
#endif

namespace updater {
namespace {
struct ConfigError : std::runtime_error { using std::runtime_error::runtime_error; };
[[noreturn]] void invalid(const toml::node& node, std::string_view message)
{
    const auto begin = node.source().begin;
    throw ConfigError(std::format("{}:{}: {}", begin.line, begin.column, message));
}

void keys(const toml::table& table, std::initializer_list<std::string_view> known)
{
    for (const auto& [key, value] : table)
        if (!std::ranges::contains(known, key.str()))
            invalid(value, "unknown configuration key '" + std::string(key.str()) + "'");
}

std::string string(const toml::node& node)
{
    const auto value = node.value<std::string>();
    if (!value || value->empty() || value->contains('\0'))
        invalid(node, "expected a nonempty string without NUL");
    return *value;
}

std::int64_t integer(const toml::node& node, std::int64_t minimum = 1,
                     std::int64_t maximum = std::numeric_limits<std::ptrdiff_t>::max())
{
    const auto* number = node.as_integer();
    if (!number || number->get() < minimum || number->get() > maximum)
        invalid(node, "integer is outside its allowed range");
    return number->get();
}

const toml::table* table(const toml::table& root, std::string_view name)
{
    const auto* node = root.get(name);
    if (!node) return nullptr;
    if (!node->is_table()) invalid(*node, "expected a table");
    return node->as_table();
}
}

Result<ServerConfig> read_server_config(const std::filesystem::path& file)
{
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error)) return std::unexpected("configuration is not a regular file");
        std::ifstream input(file, std::ios::binary);
        if (!input) return std::unexpected("cannot open server configuration");
        std::string source(1024 * 1024 + 1, '\0');
        input.read(source.data(), static_cast<std::streamsize>(source.size()));
        if (input.bad()) return std::unexpected("cannot read server configuration");
        if (input.gcount() > 1024 * 1024) return std::unexpected("server configuration exceeds 1 MiB");
        source.resize(static_cast<std::size_t>(input.gcount()));
        const auto root = toml::parse(source, file.string());
        keys(root, {"listen", "catalog_dir", "catalog_files", "publisher_id_base",
                    "state_dir", "log_dir", "limits", "retention", "timeouts"});
        const auto base = std::filesystem::absolute(file).parent_path();
        const auto path = [&](const toml::node& node) {
            auto result = std::filesystem::path(string(node));
            return result.is_absolute() ? result : (base / result).lexically_normal();
        };
        ServerConfig config;
        if (const auto* listen = root.get("listen")) {
            const auto value = string(*listen);
            const auto colon = value.rfind(':');
            if (colon == value.npos) invalid(*listen, "listen needs address:port");
            auto address = value.substr(0, colon);
            if (address.starts_with('[') && address.ends_with(']')) address = address.substr(1, address.size() - 2);
            boost::system::error_code ec;
            const auto parsed = boost::asio::ip::make_address(address, ec);
            if (ec || !parsed.is_loopback()) invalid(*listen, "listen must be a numeric loopback address");
            unsigned port{};
            const auto [end, status] = std::from_chars(value.data() + colon + 1, value.data() + value.size(), port);
            if (status != std::errc{} || end != value.data() + value.size() || port > 65535)
                invalid(*listen, "invalid listener port");
            config.http.address = std::move(address);
            config.http.port = static_cast<std::uint16_t>(port);
        }
        if (root.contains("catalog_dir") == root.contains("catalog_files"))
            invalid(root, "choose exactly one of catalog_dir and catalog_files");
        if (const auto* directory = root.get("catalog_dir")) config.catalog_directory = path(*directory);
        if (const auto* files = root.get("catalog_files")) {
            if (!files->is_array() || files->as_array()->empty()) invalid(*files, "catalog_files must be a nonempty array");
            for (const auto& entry : *files->as_array()) config.catalog_files.push_back(path(entry));
        }
        if (const auto* value = root.get("publisher_id_base")) config.feed.publisher_id_base = string(*value);
        if (const auto* value = root.get("state_dir")) config.state_directory = path(*value);
        if (const auto* value = root.get("log_dir")) config.log_directory = path(*value);
        if (const auto* limits = table(root, "limits")) {
            keys(*limits, {"connections", "unfinished_requests", "contact_log_bytes_per_file",
                "catalog_bytes", "catalog_fragments", "response_bytes", "header_bytes", "target_bytes"});
            const auto count = [&](std::string_view key, std::ptrdiff_t& destination) {
                if (const auto* value = limits->get(key)) destination = integer(*value);
            };
            count("connections", config.http.maximum_connections);
            count("unfinished_requests", config.worker.maximum_unfinished);
            count("contact_log_bytes_per_file", config.log_bytes);
            count("catalog_bytes", config.catalog_budget.bytes);
            count("catalog_fragments", config.catalog_budget.fragments);
            count("response_bytes", config.feed.maximum_bytes);
            if (const auto* value = limits->get("header_bytes"))
                config.http.header_bytes = integer(*value, 128, std::numeric_limits<std::uint32_t>::max());
            if (const auto* value = limits->get("target_bytes"))
                config.http.target_bytes = integer(*value, 1, 256);
        }
        if (const auto* retention = table(root, "retention")) {
            keys(*retention, {"activity_days", "inactive_installation_days", "batch"});
            if (const auto* value = retention->get("activity_days"))
                config.worker.retention.activity_days = integer(*value, 1, std::numeric_limits<std::int64_t>::max()/86400);
            if (const auto* value = retention->get("inactive_installation_days"))
                config.worker.retention.inactive_days = integer(*value, 1, std::numeric_limits<std::int64_t>::max()/86400);
            if (const auto* value = retention->get("batch")) config.worker.retention.batch = integer(*value);
        }
        if (const auto* times = table(root, "timeouts")) {
            keys(*times, {"header_ms", "queue_ms", "write_ms", "sqlite_busy_ms", "worker_ms", "reload_ms", "shutdown_ms"});
            const auto duration = [&](std::string_view key, std::chrono::milliseconds& destination) {
                if (const auto* value = times->get(key)) destination = std::chrono::milliseconds(integer(*value, 1, 600000));
            };
            duration("header_ms", config.http.read_timeout);
            duration("queue_ms", config.worker.queue_wait);
            duration("write_ms", config.http.write_timeout);
            duration("sqlite_busy_ms", config.sqlite_busy);
            duration("worker_ms", config.worker_budget);
            duration("reload_ms", config.reload_timeout);
            duration("shutdown_ms", config.shutdown_timeout);
        }
        using namespace std::chrono_literals;
        config.worker.idle_interval = std::clamp(config.worker_budget / 3, 1ms, 1000ms);
        return config;
    } catch (const toml::parse_error& error) {
        return std::unexpected(std::format("{}:{}: {}", file.string(), error.source().begin.line,
                                           error.description()));
    } catch (const ConfigError& error) {
        return std::unexpected(file.string() + ":" + error.what());
    } catch (const std::filesystem::filesystem_error& error) { return std::unexpected(error.what()); }
}

Result<std::unique_ptr<const Catalog>> prepare_catalog(const ServerConfig& config)
{
    auto loaded = config.catalog_directory.empty()
        ? load_catalog(config.catalog_files, config.catalog_budget)
        : load_catalog_directory(config.catalog_directory, config.catalog_budget);
    if (!loaded) {
        std::string diagnostics;
        for (const auto& diagnostic : loaded.error()) diagnostics += format_diagnostic(diagnostic) + "\n";
        return std::unexpected(std::move(diagnostics));
    }
    if (auto checked = preflight_feeds(**loaded, config.feed); !checked)
        return std::unexpected(checked.error());
    for (const auto& suite : (*loaded)->suites.keys()) {
        const auto line = contact_line({nil_uuid, suite}, WallTime{});
        if (!line || std::ssize(*line) > config.log_bytes)
            return std::unexpected("contact log limit cannot hold a catalog suite's record");
    }
    return std::move(*loaded);
}

} // namespace updater
