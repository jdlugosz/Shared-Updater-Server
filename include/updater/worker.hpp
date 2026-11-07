// Shared Updater Server - include/updater/worker.hpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#pragma once

#include "updater/processor.hpp"

namespace updater {

class AdmissionLease;
struct RequestControl;

struct RequestHandle {
    std::int64_t sequence = 0;
    std::weak_ptr<RequestControl> control;
};
struct Submission {
    RequestHandle handle;
    std::shared_ptr<AdmissionLease> lease;
    MonoTime queue_deadline{};
};
struct Completion {
    std::int64_t sequence = 0;
    Reply reply;
    std::shared_ptr<AdmissionLease> lease;
    Completion(std::int64_t sequence, Reply reply, std::shared_ptr<AdmissionLease> lease)
        : sequence(sequence), reply(std::move(reply)), lease(std::move(lease)) {}
    Completion(Completion&&) noexcept = default;
    Completion& operator=(Completion&&) noexcept = default;
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
};
using CompletionPort = std::function<void(Completion)>;
using ProcessorFactory = std::move_only_function<Result<std::unique_ptr<Processor>>()>;
using ActivationPort = std::function<void(Result<std::string>)>;

struct WorkerOptions {
    std::ptrdiff_t maximum_unfinished = 64;
    std::chrono::milliseconds queue_wait = std::chrono::seconds(5);
    std::chrono::milliseconds idle_interval = std::chrono::seconds(1);
    Retention retention{};
};

struct WorkerStatus {
    bool ready = false;
    bool running = true;
    bool accepting = false;
    bool failed = false;
    std::ptrdiff_t queued = 0;
    std::ptrdiff_t unfinished = 0;
    std::int64_t completed = 0;
    std::int64_t active_sequence = 0;
    std::string phase = "starting";
    std::string revision;
    std::string error;
    MonoTime operation_started{};
    MonoTime last_progress{};
};

enum class SubmitError { busy, stopping };
enum class CancelReason { disconnected, queue_timeout };

[[nodiscard]] bool worker_healthy(const WorkerStatus&, MonoTime now,
                                  std::chrono::milliseconds budget = std::chrono::seconds(10));

class RequestWorker {
public:
    explicit RequestWorker(ProcessorFactory factory, WorkerOptions options = {}, Clocks clocks = {});
    ~RequestWorker();
    RequestWorker(const RequestWorker&) = delete;
    RequestWorker& operator=(const RequestWorker&) = delete;

    [[nodiscard]] Result<void> wait_ready(std::chrono::milliseconds timeout = std::chrono::seconds(30));
    [[nodiscard]] bool wait_stopped(std::chrono::milliseconds timeout = std::chrono::seconds(30));
    [[nodiscard]] std::expected<Submission, SubmitError> try_submit(CheckRequest request, CompletionPort port);
    // Timeout cancels only queued work; disconnect also cancels preparation.
    bool cancel(const RequestHandle&, CancelReason);
    // Only a validated and preflighted candidate may be offered.
    bool offer_candidate(std::unique_ptr<const Catalog> candidate, ActivationPort port);
    void begin_shutdown();
    [[nodiscard]] WorkerStatus status() const;
    void wake(); // Notify after advancing an injected monotonic clock.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace updater
