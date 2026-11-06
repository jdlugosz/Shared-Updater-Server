#include "updater/service.hpp"
#include "updater/build_info.hpp"
#include "updater/json.hpp"
#include "updater/write_text.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <systemd/sd-daemon.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <future>
#include <iostream>
#include <print>
#include <set>
#include <stdexcept>
#include <thread>
#include <ctime>
#include <utility>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace updater {
namespace {
namespace net = boost::asio;
using Error = boost::system::error_code;
using namespace std::chrono_literals;
using Prepared = Result<std::unique_ptr<const Catalog>>;
struct Loaded {
    Prepared result;
    MonoTime finished_at;
};

class DirectoryLock {
public:
    explicit DirectoryLock(const std::filesystem::path& directory)
    {
        std::filesystem::create_directories(directory);
        struct stat status{};
        if (::lstat(directory.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)
            || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0)
            throw std::runtime_error("state and log directories must be private directories owned by the service user");
        const auto path = directory / "daemon.lock";
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0) throw std::runtime_error("cannot open daemon lock");
        if (::fstat(fd_, &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1
            || ::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("another daemon owns the state or log directory");
        }
    }
    ~DirectoryLock() { if (fd_ >= 0) ::close(fd_); }
private:
    int fd_ = -1;
};

class Controller {
public:
    Controller(net::io_context& context, RequestWorker& worker, const ServerConfig& config)
        : context_(context), worker_(worker), config_(config), guard_(net::make_work_guard(context)),
          server_(context, worker, config.http, [this] { return health(); }),
          signals_(context, SIGTERM, SIGINT, SIGHUP), tick_(context), reload_deadline_(context)
    {
        std::uint64_t usec{};
        watchdog_ = sd_watchdog_enabled(0, &usec) > 0;
        if (watchdog_)
            heartbeat_interval_ = std::max(1ms, std::min(5000ms, std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::microseconds(usec / 3))));
    }
    Result<void> start(std::string report)
    {
        auto started = server_.start();
        if (!started) return std::unexpected(started.error());
        std::println("{}\nListening on {}:{}", report, config_.http.address, *started);
        std::fflush(stdout);
        sd_notify(0, "READY=1\nSTATUS=Serving update checks");
        arm_signal();
        schedule_tick();
        return {};
    }
    int exit_code() const { return exit_code_; }
private:
    net::io_context& context_;
    RequestWorker& worker_;
    const ServerConfig& config_;
    net::executor_work_guard<net::io_context::executor_type> guard_;
    HttpServer server_;
    net::signal_set signals_;
    net::steady_timer tick_;
    net::steady_timer reload_deadline_;
    std::jthread loader_;
    std::future<Loaded> loaded_;
    bool stopping_ = false;
    bool forced_close_ = false;
    bool reload_busy_ = false;
    bool reload_again_ = false;
    bool reload_timed_out_ = false;
    bool watchdog_ = false;
    bool warned_stall_ = false;
    int exit_code_ = 0;
    std::int64_t generation_ = 0;
    std::string reload_state_ = "never";
    std::string last_error_;
    MonoTime stop_started_{};
    MonoTime last_heartbeat_{};
    MonoTime load_deadline_{};
    std::chrono::milliseconds heartbeat_interval_ = 5s;

    HealthReply health()
    {
        const auto status = worker_.status();
        const bool ready = !stopping_ && status.accepting
            && worker_healthy(status, std::chrono::steady_clock::now(), config_.worker_budget);
        return {ready, std::format(
            R"({{"ready":{},"revision":{},"phase":{},"queued":{},"unfinished":{},"completed":{},"reload":{},"reload_generation":{}}})",
            ready, json_quote(status.revision), json_quote(status.phase), status.queued, status.unfinished,
            status.completed, json_quote(reload_state_), generation_)};
    }
    void arm_signal()
    {
        signals_.async_wait([this](Error error, int signal) {
            if (error) return;
            if (signal == SIGHUP) {
                if (!stopping_) { request_reload(); arm_signal(); }
            } else stop(0);
        });
    }
    void request_reload()
    {
        if (reload_busy_) { reload_again_ = true; return; }
        reload_busy_ = true;
        reload_timed_out_ = false;
        reload_state_ = "loading";
        const auto generation = ++generation_;
        timespec now{};
        ::clock_gettime(CLOCK_MONOTONIC, &now);
        const auto usec = static_cast<unsigned long long>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
        sd_notifyf(0, "RELOADING=1\nMONOTONIC_USEC=%llu\nSTATUS=Validating catalog", usec);
        load_deadline_ = std::chrono::steady_clock::now() + config_.reload_timeout;
        std::promise<Loaded> promise;
        loaded_ = promise.get_future();
        loader_ = std::jthread([config = config_, promise = std::move(promise)]() mutable {
            auto load = [&]() -> Prepared {
                try { return prepare_catalog(config); }
                catch (const std::exception& error) { return std::unexpected(error.what()); }
                catch (...) { return std::unexpected("unexpected loader failure"); }
            };
            auto result = load();
            promise.set_value_at_thread_exit({std::move(result), std::chrono::steady_clock::now()});
        });
        reload_deadline_.expires_at(load_deadline_);
        reload_deadline_.async_wait([this, generation](Error error) {
            if (!error && generation == generation_ && reload_busy_ && loaded_.valid()) {
                if (loaded_.wait_for(0ms) == std::future_status::ready) {
                    collect_loader();
                    return;
                }
                reload_timed_out_ = true;
                reload_state_ = "rejected";
                std::println(stderr, "Catalog reload timed out; serving the previous revision");
                sd_notify(0, "READY=1\nSTATUS=Catalog reload timed out; previous revision retained");
            }
        });
    }
    void finish_reload(bool accepted, std::string detail = {})
    {
        reload_busy_ = false;
        reload_state_ = accepted ? "accepted" : "rejected";
        if (!detail.empty()) {
            auto* output = accepted ? stdout : stderr;
            std::println(output, "{}", detail);
            std::fflush(output);
        }
        sd_notify(0, accepted ? "READY=1\nSTATUS=Catalog activated"
                             : "READY=1\nSTATUS=Catalog rejected; previous revision retained");
        if (std::exchange(reload_again_, false) && !stopping_) request_reload();
    }
    void collect_loader()
    {
        if (!loaded_.valid() || loaded_.wait_for(0ms) != std::future_status::ready) return;
        loader_.join(); // The promise becomes ready at thread exit, not before.
        // Join the exited thread before reading its stored result.
        auto outcome = loaded_.get();
        auto loaded = std::move(outcome.result);
        reload_deadline_.cancel();
        if (stopping_) { reload_busy_ = false; return; }
        if (reload_timed_out_ || outcome.finished_at > load_deadline_) {
            finish_reload(false, "Catalog validation exceeded its deadline; previous revision retained");
            return;
        }
        if (!loaded) { finish_reload(false, "Catalog reload rejected: " + loaded.error()); return; }
        auto report = format_success(**loaded);
        const auto generation = generation_;
        const auto accepted = worker_.offer_candidate(std::move(*loaded),
            [this, generation, report = std::move(report)](Result<std::string> result) mutable {
                net::post(context_, [this, generation, report = std::move(report), result = std::move(result)]() mutable {
                    if (stopping_ || generation != generation_) return;
                    finish_reload(result.has_value(),
                        result ? std::move(report) : "Catalog activation rejected: " + result.error());
                });
            });
        if (!accepted) finish_reload(false, "Catalog activation unavailable");
    }
    void stop(int code)
    {
        exit_code_ = std::max(exit_code_, code);
        if (stopping_) return;
        stopping_ = true;
        stop_started_ = std::chrono::steady_clock::now();
        sd_notify(0, "STOPPING=1\nSTATUS=Draining update checks");
        reload_deadline_.cancel();
        worker_.begin_shutdown();
        server_.stop(true);
    }
    void schedule_tick()
    {
        tick_.expires_after(100ms);
        tick_.async_wait([this](Error error) { if (!error) tick(); });
    }
    void tick()
    {
        collect_loader();
        const auto now = std::chrono::steady_clock::now();
        const auto status = worker_.status();
        if (status.failed && !stopping_) {
            std::println(stderr, "Worker failed: {}", status.error);
            stop(1);
        }
        if (!status.error.empty() && status.error != last_error_) {
            last_error_ = status.error;
            std::println(stderr, "Worker status: {}", status.error);
        }
        const bool healthy = worker_healthy(status, now, config_.worker_budget);
        if (!healthy && !warned_stall_ && !stopping_) {
            warned_stall_ = true;
            std::println(stderr, "Worker is not making healthy progress");
        }
        if (healthy) {
            warned_stall_ = false;
            if (watchdog_ && now - last_heartbeat_ >= heartbeat_interval_) {
                sd_notify(0, "WATCHDOG=1");
                last_heartbeat_ = now;
            }
        }
        if (stopping_) {
            if (!forced_close_ && now - stop_started_ >= config_.shutdown_timeout) {
                forced_close_ = true;
                server_.stop(false);
            }
            if (!status.running && !loaded_.valid() && server_.sessions() == 0) {
                signals_.cancel();
                reload_deadline_.cancel();
                guard_.reset();
                return;
            }
            if (now - stop_started_ >= config_.shutdown_timeout + 5s) {
                std::println(stderr, "Shutdown hard deadline exceeded");
                std::fflush(stderr);
                std::_Exit(1);
            }
        }
        schedule_tick();
    }
};

} // namespace

int run_service(ServerConfig config, std::unique_ptr<const Catalog> catalog)
{
    write_text(std::cout, build_info.describe()).put('\n').flush();
    ::umask(0077);
    // systemd DynamicUser exposes private StateDirectory/LogsDirectory through
    // manager-owned symlinks. Validate and lock their resolved directories.
    config.state_directory = std::filesystem::weakly_canonical(config.state_directory);
    config.log_directory = std::filesystem::weakly_canonical(config.log_directory);
    std::set<std::filesystem::path> directories{
        config.state_directory, config.log_directory};
    std::vector<std::unique_ptr<DirectoryLock>> locks;
    for (const auto& directory : directories) locks.push_back(std::make_unique<DirectoryLock>(directory));
    const auto report = format_success(*catalog);
    RequestWorker worker([config, catalog = std::move(catalog)]() mutable -> Result<std::unique_ptr<Processor>> {
        auto log = RollingContactLog::open(config.log_directory, config.log_bytes);
        if (!log) return std::unexpected(log.error());
        auto census = SqliteCensus::open(config.state_directory / "census.sqlite", config.sqlite_busy);
        if (!census) return std::unexpected(census.error());
        return std::make_unique<CheckProcessor>(std::move(catalog), config.feed,
                                               std::move(*log), std::move(*census));
    }, config.worker);
    if (auto ready = worker.wait_ready(); !ready) throw std::runtime_error(ready.error());
    net::io_context context;
    Controller controller(context, worker, config);
    if (auto started = controller.start(report); !started) throw std::runtime_error(started.error());
    context.run();
    return controller.exit_code();
}

} // namespace updater
