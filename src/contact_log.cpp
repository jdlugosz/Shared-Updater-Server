// Shared Updater Server - src/contact_log.cpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#include "updater/persistence.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <cstring>
#include <format>
#include <utility>
#include <algorithm>
#endif

namespace updater {
namespace {

std::string failure(std::string_view operation) { return std::format("{}: {}", operation, std::strerror(errno)); }

class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}

    ~Fd() { if (value_ >= 0) ::close(value_); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}

    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }

    int get() const { return value_; }
private:
    int value_;
};

Result<void> check_file(int directory, const char* name, std::ptrdiff_t limit, bool absent_ok)
{
    struct stat status{};
    if (::fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (absent_ok && errno == ENOENT) return {};
        return std::unexpected(failure("inspect log"));
    }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1 || status.st_size > limit)
        return std::unexpected("log must be a single regular file within its byte limit");
    return {};
}

} // namespace

Result<std::string> contact_line(const CheckRequest& request, WallTime observed_at)
{
    const auto id = uuid_string(request.installation_id);
    const auto target = std::format("/updchk/{}/{}", id, request.suite);
    const auto validated = parse_check_target(target);
    if (!validated || validated->suite != request.suite || observed_at.time_since_epoch().count() < 0)
        return std::unexpected("invalid contact");
    return std::format(R"({{"at":"{:%FT%TZ}","id":"{}","suite":"{}"}})" "\n",
                       observed_at, id, request.suite);
}

struct RollingContactLog::Impl {
    Fd directory;
    Fd current;
    std::ptrdiff_t limit;
    std::ptrdiff_t size = 0;
    bool broken = false;
    LogOperations operations;

    Result<void> open_current()
    {
        current = Fd(::openat(directory.get(), "current.jsonl",
                             O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (current.get() < 0) return std::unexpected(failure("open current log"));
        struct stat status{};
        if (::fstat(current.get(), &status) != 0) return std::unexpected(failure("inspect current log"));
        if (!S_ISREG(status.st_mode) || status.st_nlink != 1 || status.st_size > limit)
            return std::unexpected("current log is not a bounded regular file");
        if (::fchmod(current.get(), 0600) != 0) return std::unexpected(failure("protect current log"));
        return recover();
    }

    Result<void> recover()
    {
        struct stat status{};
        if (::fstat(current.get(), &status) != 0) return std::unexpected(failure("inspect current log"));
        if (status.st_size > limit) return std::unexpected("current log exceeds its byte limit");
        size = status.st_size;
        auto offset = size;
        std::array<char, 4096> buffer{};
        while (offset > 0) {
            const auto count = std::min(offset, std::ssize(buffer));
            const auto start = offset - count;
            std::ptrdiff_t read = 0;
            while (read < count) {
                const auto n = ::pread(current.get(), buffer.data() + read,
                                       static_cast<std::size_t>(count - read), start + read);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return std::unexpected(failure("read log tail"));
                read += n;
            }
            const std::string_view chunk(buffer.data(), static_cast<std::size_t>(count));
            if (const auto newline = chunk.rfind('\n'); newline != chunk.npos) {
                offset = start + static_cast<std::ptrdiff_t>(newline) + 1;
                break;
            }
            offset = start;
        }
        if (offset != size) {
            if (::ftruncate(current.get(), offset) != 0) return std::unexpected(failure("repair log tail"));
            size = offset;
        }
        if (operations.sync(current.get()) != 0) return std::unexpected(failure("sync recovered log"));
        broken = false;
        return {};
    }
};

RollingContactLog::RollingContactLog(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RollingContactLog::~RollingContactLog() = default;

Result<std::unique_ptr<RollingContactLog>> RollingContactLog::open(
    const std::filesystem::path& directory, std::ptrdiff_t limit, LogOperations operations)
{
    if (limit <= 0) return std::unexpected("log byte limit must be positive");
    if (!operations.write) operations.write = [](int fd, std::span<const std::byte> bytes) {
        return ::write(fd, bytes.data(), bytes.size());
    };
    if (!operations.sync) operations.sync = [](int fd) { return ::fsync(fd); };
    if (!operations.rotate) operations.rotate = [](int fd) {
        return ::renameat(fd, "current.jsonl", fd, "previous.jsonl");
    };
    auto impl = std::make_unique<Impl>();
    impl->limit = limit;
    impl->operations = std::move(operations);
    impl->directory = Fd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (impl->directory.get() < 0) return std::unexpected(failure("open log directory"));
    if (auto checked = check_file(impl->directory.get(), "previous.jsonl", limit, true); !checked)
        return std::unexpected(checked.error());
    if (auto opened = impl->open_current(); !opened) return std::unexpected(opened.error());
    if (impl->operations.sync(impl->directory.get()) != 0)
        return std::unexpected(failure("sync log directory"));
    return std::unique_ptr<RollingContactLog>(new RollingContactLog(std::move(impl)));
}

Result<void> RollingContactLog::append(const CheckRequest& request, WallTime observed_at)
{
    auto line = contact_line(request, observed_at);
    if (!line) return std::unexpected(line.error());
    auto& state = *impl_;
    if (std::ssize(*line) > state.limit) return std::unexpected("contact exceeds log byte limit");
    if (state.broken) {
        if (auto repaired = state.open_current(); !repaired) return repaired;
        state.broken = true;
        if (state.operations.sync(state.directory.get()) != 0)
            return std::unexpected(failure("sync recovered log directory"));
        state.broken = false;
    }
    if (std::ssize(*line) > state.limit - state.size) {
        state.broken = true;
        state.current = Fd{};
        if (auto checked = check_file(state.directory.get(), "previous.jsonl", state.limit, true); !checked)
            return checked;
        if (state.operations.rotate(state.directory.get()) != 0)
            return std::unexpected(failure("rotate contact log"));
        if (auto opened = state.open_current(); !opened) return opened;
        // Mark recovery necessary on any subsequent directory/write/sync error.
        state.broken = true;
        if (state.operations.sync(state.directory.get()) != 0)
            return std::unexpected(failure("sync rotated log directory"));
    }
    state.broken = true;
    auto remaining = std::as_bytes(std::span(line->data(), line->size()));
    while (!remaining.empty()) {
        const auto written = state.operations.write(state.current.get(), remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0 || written > std::ssize(remaining)) return std::unexpected("contact log write failed");
        state.size += written;
        remaining = remaining.subspan(static_cast<std::size_t>(written));
    }
    if (state.operations.sync(state.current.get()) != 0)
        return std::unexpected(failure("sync contact log"));
    state.broken = false;
    return {};
}

} // namespace updater
