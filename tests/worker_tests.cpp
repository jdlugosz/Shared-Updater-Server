#include "updater/worker.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <barrier>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace {
using namespace updater;
using namespace std::chrono_literals;

class Mailbox {
public:
    CompletionPort port() { return [this](Completion completion) {
        { std::lock_guard lock(mutex); messages.push_back(std::move(completion)); }
        changed.notify_all();
    }; }
    std::vector<Completion> take(std::size_t count)
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 3s, [&] { return messages.size() >= count; }));
        std::vector<Completion> result;
        while (count--) {
            result.push_back(std::move(messages.front()));
            messages.pop_front();
        }
        return result;
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Completion> messages;
};

struct Shared {
    std::mutex mutex;
    std::condition_variable changed;
    bool before = false;
    bool after = false;
    bool release_before = false;
    bool release_after = false;
    bool fail_activation = false;
    int active = 0;
    int peak = 0;
    std::string revision = "initial";
    std::vector<std::int64_t> started;
    std::vector<std::int64_t> persisted;
    void release() {
        { std::lock_guard lock(mutex); release_before = release_after = true; }
        changed.notify_all();
    }
    void wait_started(std::size_t count = 1) {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 3s, [&] { return started.size() >= count; }));
    }
};

class ControlledProcessor final : public Processor {
public:
    explicit ControlledProcessor(std::shared_ptr<Shared> state) : state(std::move(state)) {}
    Result<void> initialize() override { return {}; }
    Reply process(const CheckRequest&, const RequestContext& context, const std::function<bool()>& gate) override
    {
        {
            std::unique_lock lock(state->mutex);
            state->started.push_back(context.sequence);
            state->peak = std::max(state->peak, ++state->active);
            state->changed.notify_all();
            if (context.sequence == 1 && state->before)
                state->changed.wait(lock, [&] { return state->release_before; });
        }
        const bool persisted = gate();
        std::string revision;
        {
            std::unique_lock lock(state->mutex);
            if (persisted) state->persisted.push_back(context.sequence);
            state->changed.notify_all();
            if (context.sequence == 1 && state->after)
                state->changed.wait(lock, [&] { return state->release_after; });
            revision = state->revision;
            --state->active;
        }
        return {persisted ? Outcome::ok : Outcome::unavailable, std::move(revision), persisted ? "" : "cancelled"};
    }
    Result<void> activate(std::unique_ptr<const Catalog> candidate) override
    {
        std::lock_guard lock(state->mutex);
        if (state->fail_activation) return std::unexpected("injected");
        state->revision = candidate->header.revision;
        return {};
    }
    Result<bool> maintain(WallTime, const Retention&) override { return false; }
    std::string revision() const override { std::lock_guard lock(state->mutex); return state->revision; }
    std::shared_ptr<Shared> state;
};

class Fixture {
public:
    explicit Fixture(WorkerOptions options = {}, Clocks clocks = {}, bool before = true, bool after = false)
    {
        state->before = before;
        state->after = after;
        worker = std::make_unique<RequestWorker>([shared = state]() -> Result<std::unique_ptr<Processor>> {
            return std::make_unique<ControlledProcessor>(shared);
        }, options, std::move(clocks));
        REQUIRE(worker->wait_ready());
    }
    ~Fixture() { state->release(); worker->begin_shutdown(); worker.reset(); }
    Submission submit() {
        auto submitted = worker->try_submit({nil_uuid, "launcher"}, mailbox.port());
        REQUIRE(submitted);
        return std::move(*submitted);
    }
    std::shared_ptr<Shared> state = std::make_shared<Shared>();
    Mailbox mailbox;
    std::unique_ptr<RequestWorker> worker;
};
}

TEST_CASE("One worker processes simultaneous producers in successful admission order", "[worker]")
{
    Fixture fixture({.maximum_unfinished = 32});
    std::vector<Submission> submissions;
    submissions.push_back(fixture.submit());
    fixture.state->wait_started();
    std::barrier start(17);
    std::mutex results;
    std::atomic<int> refused{};
    std::vector<std::jthread> producers;
    for (int n = 0; n < 16; ++n) producers.emplace_back([&] {
        start.arrive_and_wait();
        auto submitted = fixture.worker->try_submit({nil_uuid, "launcher"}, fixture.mailbox.port());
        if (!submitted) { ++refused; return; }
        std::lock_guard lock(results);
        submissions.push_back(std::move(*submitted));
    });
    start.arrive_and_wait();
    producers.clear();
    REQUIRE(refused == 0);
    REQUIRE(fixture.worker->status().queued == 16);
    fixture.state->release();
    auto completed = fixture.mailbox.take(17);
    REQUIRE(fixture.state->peak == 1);
    REQUIRE(std::ranges::is_sorted(fixture.state->started));
    REQUIRE(fixture.state->started.size() == 17);
    REQUIRE(fixture.state->persisted == fixture.state->started);
    submissions.clear();
    completed.clear();
    REQUIRE(fixture.worker->status().unfinished == 0);
}

TEST_CASE("Completed replies retain admission capacity until delivery releases them", "[worker]")
{
    Fixture fixture({.maximum_unfinished = 2});
    auto first = fixture.submit();
    fixture.state->wait_started();
    auto second = fixture.submit();
    REQUIRE(fixture.worker->try_submit({nil_uuid, "launcher"}, fixture.mailbox.port()).error() == SubmitError::busy);
    fixture.state->release();
    auto completed = fixture.mailbox.take(2);
    REQUIRE(fixture.worker->status().queued == 0);
    REQUIRE(fixture.worker->status().unfinished == 2);
    REQUIRE_FALSE(fixture.worker->try_submit({nil_uuid, "launcher"}, fixture.mailbox.port()));
    first.lease.reset();
    completed[0].lease.reset();
    REQUIRE(fixture.worker->status().unfinished == 1);
    auto third = fixture.submit();
    auto last = fixture.mailbox.take(1);
}

TEST_CASE("Cancellation removes queued messages and wins before the persistence gate", "[worker]")
{
    Fixture fixture({.maximum_unfinished = 2});
    auto first = fixture.submit();
    fixture.state->wait_started();
    for (int n = 0; n < 32; ++n) {
        auto queued = fixture.submit();
        REQUIRE(fixture.worker->cancel(queued.handle, CancelReason::disconnected));
        REQUIRE_FALSE(fixture.worker->cancel(queued.handle, CancelReason::disconnected));
        queued.lease.reset();
        auto discarded = fixture.mailbox.take(1);
        REQUIRE(discarded.front().reply.category == "cancelled");
        discarded.clear();
        REQUIRE(fixture.worker->status().queued == 0);
        REQUIRE(fixture.worker->status().unfinished == 1);
    }
    REQUIRE(fixture.worker->cancel(first.handle, CancelReason::disconnected));
    fixture.state->release();
    auto completed = fixture.mailbox.take(1);
    REQUIRE(completed.front().reply.category == "cancelled");
    REQUIRE(fixture.state->persisted.empty());
}

TEST_CASE("Exact queue expiry cannot expire a request already preparing", "[worker]")
{
    std::atomic<std::int64_t> seconds{};
    Clocks clocks{[] { return test::published().seconds; },
                  [&] { return MonoTime{} + std::chrono::seconds(seconds.load()); }};
    Fixture fixture({.maximum_unfinished = 2}, clocks);
    auto first = fixture.submit();
    fixture.state->wait_started();
    auto second = fixture.submit();
    seconds = 5;
    REQUIRE_FALSE(fixture.worker->cancel(first.handle, CancelReason::queue_timeout));
    fixture.state->release();
    auto completed = fixture.mailbox.take(2);
    REQUIRE(completed[0].reply.outcome == Outcome::ok);
    REQUIRE(completed[1].reply.category == "queue_timeout");
    REQUIRE(fixture.state->persisted == std::vector<std::int64_t>{1});
}

TEST_CASE("Disconnect after persistence begins cannot release an active reservation", "[worker]")
{
    Fixture fixture({.maximum_unfinished = 1}, {}, false, true);
    auto first = fixture.submit();
    {
        std::unique_lock lock(fixture.state->mutex);
        REQUIRE(fixture.state->changed.wait_for(lock, 3s, [&] { return !fixture.state->persisted.empty(); }));
    }
    REQUIRE_FALSE(fixture.worker->cancel(first.handle, CancelReason::disconnected));
    first.lease.reset();
    REQUIRE(fixture.worker->status().unfinished == 1);
    REQUIRE_FALSE(fixture.worker->try_submit({nil_uuid, "launcher"}, fixture.mailbox.port()));
    fixture.state->release();
    auto completed = fixture.mailbox.take(1);
    REQUIRE(completed.front().reply.outcome == Outcome::ok);
    completed.clear();
    REQUIRE(fixture.worker->status().unfinished == 0);
}

TEST_CASE("Catalog activation uses its control slot between queued checks", "[worker]")
{
    Fixture fixture({.maximum_unfinished = 2});
    SECTION("valid activation") {}
    SECTION("activation failure") { fixture.state->fail_activation = true; }
    auto first = fixture.submit();
    fixture.state->wait_started();
    auto second = fixture.submit();
    std::promise<Result<std::string>> activation;
    auto activated = activation.get_future();
    REQUIRE(fixture.worker->offer_candidate(test::catalog("next"),
        [&](Result<std::string> result) { activation.set_value(std::move(result)); }));
    REQUIRE_FALSE(fixture.worker->offer_candidate(test::catalog("extra"), [](auto) {}));
    fixture.state->release();
    auto completed = fixture.mailbox.take(2);
    REQUIRE(activated.wait_for(3s) == std::future_status::ready);
    REQUIRE(activated.get().has_value() == !fixture.state->fail_activation);
    REQUIRE(completed[0].reply.body == "initial");
    REQUIRE(completed[1].reply.body == (fixture.state->fail_activation ? "initial" : "next"));
}

TEST_CASE("Worker shutdown removes queued work and wakes an idle thread", "[worker]")
{
    Fixture fixture;
    auto first = fixture.submit();
    fixture.state->wait_started();
    auto queued = fixture.submit();
    fixture.worker->begin_shutdown();
    REQUIRE(fixture.worker->try_submit({nil_uuid, "launcher"}, fixture.mailbox.port()).error() == SubmitError::stopping);
    fixture.state->release();
    auto completed = fixture.mailbox.take(2);
    REQUIRE(fixture.worker->wait_stopped(3s));
    REQUIRE(fixture.state->persisted.empty());
    first.lease.reset();
    queued.lease.reset();
    completed.clear();
    REQUIRE(fixture.worker->status().unfinished == 0);
}

TEST_CASE("Worker startup failure is reported and cannot accept work", "[worker]")
{
    RequestWorker worker([]() -> Result<std::unique_ptr<Processor>> { return std::unexpected("injected"); });
    REQUIRE_FALSE(worker.wait_ready());
    REQUIRE(worker.status().failed);
    REQUIRE(worker.wait_stopped(3s));
}

TEST_CASE("Watchdog health distinguishes idle overload and a stuck operation", "[worker]")
{
    const auto now = std::chrono::steady_clock::now();
    WorkerStatus status;
    status.ready = status.accepting = true;
    status.phase = "idle";
    status.last_progress = status.operation_started = now;
    REQUIRE(worker_healthy(status, now));
    status.queued = status.unfinished = 64;
    REQUIRE(worker_healthy(status, now));
    status.phase = "persisting";
    status.queued = 0;
    status.operation_started = now - 11s;
    REQUIRE_FALSE(worker_healthy(status, now));
    status.phase = "idle";
    status.last_progress = now - 11s;
    REQUIRE_FALSE(worker_healthy(status, now));
    status.last_progress = now;
    status.failed = true;
    REQUIRE_FALSE(worker_healthy(status, now));
}
