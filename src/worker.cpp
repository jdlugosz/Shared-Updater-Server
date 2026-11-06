#include "updater/worker.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace updater {
namespace {
struct Capacity {
    std::mutex mutex;
    std::ptrdiff_t unfinished = 0;
};
enum class Phase { queued, preparing, persisting, finished };
}

class AdmissionLease {
public:
    explicit AdmissionLease(std::shared_ptr<Capacity> capacity) : capacity_(std::move(capacity)) {}
    ~AdmissionLease() {
        if (reserved) {
            std::lock_guard lock(capacity_->mutex);
            --capacity_->unfinished;
        }
    }
    bool reserved = false; // Set exactly once before admission becomes visible.
private:
    std::shared_ptr<Capacity> capacity_;
};

struct RequestControl {
    std::int64_t sequence = 0;
    Phase phase = Phase::queued;
    bool cancelled = false;
};

struct RequestWorker::Impl {
    struct Job {
        CheckRequest request;
        RequestContext context;
        CompletionPort port;
        std::shared_ptr<RequestControl> control;
        std::shared_ptr<AdmissionLease> lease;
    };
    struct Candidate {
        std::unique_ptr<const Catalog> catalog;
        ActivationPort port;
    };

    std::shared_ptr<Capacity> capacity = std::make_shared<Capacity>();
    std::condition_variable changed;
    WorkerOptions options;
    Clocks clocks;
    ProcessorFactory factory;
    std::deque<Job> queue;
    std::shared_ptr<RequestControl> active;
    std::optional<Candidate> candidate;
    WorkerStatus state;
    bool stopping = false;
    bool last_was_activation = false;
    bool clock_changed = false;
    std::int64_t next_sequence = 1;
    std::jthread thread; // Destroyed before all state it uses.

    Impl(ProcessorFactory factory, WorkerOptions options, Clocks clocks)
        : options(options), clocks(std::move(clocks)), factory(std::move(factory))
    {
        if (options.maximum_unfinished <= 0 || options.queue_wait <= std::chrono::milliseconds::zero()
            || options.idle_interval <= std::chrono::milliseconds::zero())
            throw std::invalid_argument("invalid worker limits");
        thread = std::jthread([this] { run(); });
    }

    void deliver(Job job, Reply reply)
    {
        job.port(Completion(job.context.sequence, std::move(reply), std::move(job.lease)));
    }

    void run() noexcept
    {
        try {
            auto created = factory();
            factory = nullptr;
            if (!created) throw std::runtime_error(created.error());
            auto processor = std::move(*created);
            if (!processor) throw std::runtime_error("missing processor");
            if (auto initialized = processor->initialize(); !initialized)
                throw std::runtime_error(initialized.error());
            {
                std::lock_guard lock(capacity->mutex);
                state.ready = true;
                state.accepting = !stopping;
                state.phase = "idle";
                state.revision = processor->revision();
                state.last_progress = clocks.monotonic();
            }
            changed.notify_all();
            std::int64_t maintenance_day = -1;
            bool more_maintenance = false;
            auto next_maintenance = MonoTime::min();
            std::ptrdiff_t since_maintenance = 0;
            for (;;) {
                std::optional<Job> job;
                std::optional<Candidate> activation;
                bool maintenance = false;
                bool expired = false;
                {
                    std::unique_lock lock(capacity->mutex);
                    changed.wait_for(lock, options.idle_interval, [this] {
                        return stopping || !queue.empty() || candidate.has_value() || clock_changed;
                    });
                    clock_changed = false;
                    state.last_progress = clocks.monotonic();
                    if (stopping) break;
                    if (candidate && (!last_was_activation || queue.empty())) {
                        activation = std::move(candidate);
                        candidate.reset();
                        state.phase = "activation";
                    } else {
                        const auto day = clocks.wall().time_since_epoch().count() / 86400;
                        if ((more_maintenance || day > maintenance_day) && clocks.monotonic() >= next_maintenance
                            && (queue.empty() || since_maintenance >= 100)) {
                            maintenance = true;
                            state.phase = "maintenance";
                        } else if (!queue.empty()) {
                            job = std::move(queue.front());
                            queue.pop_front();
                            active = job->control;
                            expired = clocks.monotonic() >= job->context.queue_deadline;
                            active->phase = Phase::preparing;
                            state.active_sequence = job->context.sequence;
                            state.phase = "preparing";
                        } else {
                            state.phase = "idle";
                            continue;
                        }
                    }
                    state.operation_started = clocks.monotonic();
                }
                if (activation) {
                    auto activated = processor->activate(std::move(activation->catalog));
                    const auto revision = processor->revision();
                    {
                        std::lock_guard lock(capacity->mutex);
                        state.revision = revision;
                        state.phase = "idle";
                        state.last_progress = clocks.monotonic();
                        last_was_activation = true;
                    }
                    activation->port(activated ? Result<std::string>(revision) : std::unexpected(activated.error()));
                } else if (maintenance) {
                    auto maintained = processor->maintain(clocks.wall(), options.retention);
                    more_maintenance = maintained && *maintained;
                    maintenance_day = clocks.wall().time_since_epoch().count() / 86400;
                    next_maintenance = clocks.monotonic() + (maintained ? std::chrono::seconds(1) : std::chrono::minutes(1));
                    since_maintenance = 0;
                    std::lock_guard lock(capacity->mutex);
                    state.phase = "idle";
                    state.last_progress = clocks.monotonic();
                    if (!maintained) state.error = "maintenance";
                } else if (job) {
                    Reply reply;
                    if (expired) {
                        reply = {Outcome::unavailable, {}, "queue_timeout"};
                    } else {
                        const auto control = job->control;
                        reply = processor->process(job->request, job->context, [this, control] {
                            std::lock_guard lock(capacity->mutex);
                            if (control->cancelled || stopping) return false;
                            control->phase = Phase::persisting;
                            state.phase = "persisting";
                            return true;
                        });
                    }
                    {
                        std::lock_guard lock(capacity->mutex);
                        job->control->phase = Phase::finished;
                        active.reset();
                        state.active_sequence = 0;
                        state.phase = "idle";
                        state.last_progress = clocks.monotonic();
                        ++state.completed;
                        if (reply.category == "log" || reply.category == "census" || reply.category == "feed")
                            state.error = "check " + std::to_string(job->context.sequence) + ": "
                                + reply.category + " " + reply.detail;
                        last_was_activation = false;
                    }
                    since_maintenance = std::min(since_maintenance + 1, std::ptrdiff_t{100});
                    deliver(std::move(*job), std::move(reply));
                }
            }
            processor.reset(); // Durable resources close on their owning worker.
            {
                std::lock_guard lock(capacity->mutex);
                state.running = false;
                state.accepting = false;
                state.phase = "stopped";
                state.last_progress = clocks.monotonic();
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(capacity->mutex);
            state.failed = true;
            state.running = false;
            state.accepting = false;
            state.error = error.what();
        } catch (...) {
            std::lock_guard lock(capacity->mutex);
            state.failed = true;
            state.running = false;
            state.accepting = false;
            state.error = "unexpected worker failure";
        }
        changed.notify_all();
    }
};

RequestWorker::RequestWorker(ProcessorFactory factory, WorkerOptions options, Clocks clocks)
    : impl_(std::make_unique<Impl>(std::move(factory), options, std::move(clocks))) {}
RequestWorker::~RequestWorker() { begin_shutdown(); }

Result<void> RequestWorker::wait_ready(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(impl_->capacity->mutex);
    if (!impl_->changed.wait_for(lock, timeout, [this] { return impl_->state.ready || !impl_->state.running; }))
        return std::unexpected("worker startup timeout");
    if (impl_->state.failed) return std::unexpected(impl_->state.error);
    return {};
}

bool RequestWorker::wait_stopped(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(impl_->capacity->mutex);
    return impl_->changed.wait_for(lock, timeout, [this] { return !impl_->state.running; });
}

std::expected<Submission, SubmitError> RequestWorker::try_submit(CheckRequest request, CompletionPort port)
{
    if (!port) throw std::invalid_argument("missing completion port");
    auto control = std::make_shared<RequestControl>();
    auto lease = std::make_shared<AdmissionLease>(impl_->capacity);
    MonoTime deadline;
    {
        std::lock_guard lock(impl_->capacity->mutex);
        if (!impl_->state.accepting || impl_->next_sequence == std::numeric_limits<std::int64_t>::max())
            return std::unexpected(SubmitError::stopping);
        if (impl_->capacity->unfinished >= impl_->options.maximum_unfinished)
            return std::unexpected(SubmitError::busy);
        RequestContext context{impl_->next_sequence, impl_->clocks.wall(), impl_->clocks.monotonic(), {}};
        context.queue_deadline = context.admitted_at + impl_->options.queue_wait;
        deadline = context.queue_deadline;
        control->sequence = context.sequence;
        impl_->queue.push_back({std::move(request), context, std::move(port), control, lease});
        ++impl_->next_sequence;
        ++impl_->capacity->unfinished;
        lease->reserved = true;
    }
    impl_->changed.notify_one();
    return Submission{{control->sequence, control}, std::move(lease), deadline};
}

bool RequestWorker::cancel(const RequestHandle& handle, CancelReason reason)
{
    std::optional<Impl::Job> removed;
    {
        std::lock_guard lock(impl_->capacity->mutex);
        const auto control = handle.control.lock();
        if (!control || control->sequence != handle.sequence) return false;
        if (control->phase == Phase::queued) {
            const auto found = std::ranges::find_if(impl_->queue, [&](const auto& job) { return job.control == control; });
            if (found == impl_->queue.end()) return false;
            if (reason == CancelReason::queue_timeout && impl_->clocks.monotonic() < found->context.queue_deadline)
                return false;
            removed = std::move(*found);
            impl_->queue.erase(found);
            control->phase = Phase::finished;
            ++impl_->state.completed;
        } else if (control->phase == Phase::preparing && reason != CancelReason::queue_timeout) {
            control->cancelled = true;
            return true;
        } else return false;
    }
    impl_->deliver(std::move(*removed), {Outcome::unavailable, {},
        reason == CancelReason::queue_timeout ? "queue_timeout" : "cancelled"});
    return true;
}

bool RequestWorker::offer_candidate(std::unique_ptr<const Catalog> candidate, ActivationPort port)
{
    if (!candidate || !port) return false;
    {
        std::lock_guard lock(impl_->capacity->mutex);
        if (!impl_->state.accepting || impl_->candidate || impl_->state.phase == "activation") return false;
        impl_->candidate.emplace(Impl::Candidate{std::move(candidate), std::move(port)});
    }
    impl_->changed.notify_one();
    return true;
}

void RequestWorker::begin_shutdown()
{
    std::deque<Impl::Job> removed;
    std::optional<Impl::Candidate> candidate;
    {
        std::lock_guard lock(impl_->capacity->mutex);
        if (impl_->stopping) return;
        impl_->stopping = true;
        impl_->state.accepting = false;
        removed.swap(impl_->queue);
        candidate = std::move(impl_->candidate);
        impl_->candidate.reset();
        for (auto& job : removed) {
            job.control->phase = Phase::finished;
            ++impl_->state.completed;
        }
        if (impl_->active && impl_->active->phase == Phase::preparing) impl_->active->cancelled = true;
    }
    impl_->changed.notify_all();
    for (auto& job : removed) impl_->deliver(std::move(job), {Outcome::unavailable, {}, "stopping"});
    if (candidate) candidate->port(std::unexpected("stopping"));
}

WorkerStatus RequestWorker::status() const
{
    std::lock_guard lock(impl_->capacity->mutex);
    auto result = impl_->state;
    result.queued = std::ssize(impl_->queue);
    result.unfinished = impl_->capacity->unfinished;
    return result;
}

void RequestWorker::wake()
{
    {
        std::lock_guard lock(impl_->capacity->mutex);
        impl_->clock_changed = true;
    }
    impl_->changed.notify_all();
}

bool worker_healthy(const WorkerStatus& status, MonoTime now, std::chrono::milliseconds budget)
{
    if (!status.ready || !status.running || status.failed || now - status.last_progress > budget) return false;
    return status.phase == "idle" || now - status.operation_started <= budget;
}

} // namespace updater
