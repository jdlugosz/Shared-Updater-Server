// Shared Updater Server - include/updater/persistence.hpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#pragma once

#include "updater/catalog.hpp"
#include "updater/request.hpp"
#include "updater/result.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>

namespace updater {

struct Retention {
    std::int64_t activity_days = 400;
    std::int64_t inactive_days = 730;
    std::ptrdiff_t batch = 256;
};

class ContactSink {
public:
    virtual ~ContactSink() = default;
    virtual Result<void> append(const CheckRequest& request, WallTime observed_at) = 0;
};

class CensusSink {
public:
    virtual ~CensusSink() = default;
    virtual Result<void> register_suites(const Catalog& catalog) = 0;
    virtual Result<void> record(const CheckRequest& request, WallTime observed_at) = 0;
    virtual Result<bool> prune(WallTime now, const Retention& retention) = 0;
};

// Optional file-operation adapters support fault testing without production routes.
struct LogOperations {
    std::function<std::ptrdiff_t(int, std::span<const std::byte>)> write;
    std::function<int(int)> sync;
    std::function<int(int)> rotate; // directory fd: current.jsonl -> previous.jsonl
};

class RollingContactLog final : public ContactSink {
public:
    static Result<std::unique_ptr<RollingContactLog>> open(
        const std::filesystem::path& directory, std::ptrdiff_t bytes_per_file = 1024 * 1024,
        LogOperations operations = {});
    ~RollingContactLog() override;
    Result<void> append(const CheckRequest& request, WallTime observed_at) override;
private:
    struct Impl;
    explicit RollingContactLog(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class SqliteCensus final : public CensusSink {
public:
    static Result<std::unique_ptr<SqliteCensus>> open(
        const std::filesystem::path& database, std::chrono::milliseconds busy_timeout = std::chrono::seconds(2));
    ~SqliteCensus() override;
    Result<void> register_suites(const Catalog& catalog) override;
    Result<void> record(const CheckRequest& request, WallTime observed_at) override;
    Result<bool> prune(WallTime now, const Retention& retention) override;
private:
    struct Impl;
    explicit SqliteCensus(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] Result<std::string> contact_line(const CheckRequest& request, WallTime observed_at);

} // namespace updater
