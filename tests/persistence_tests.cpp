// Shared Updater Server - tests/persistence_tests.cpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#include "updater/processor.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace updater;
using namespace std::chrono_literals;

class Database {
public:
    explicit Database(const std::filesystem::path& path) {
        REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        exec("PRAGMA foreign_keys=ON");
    }

    ~Database() { sqlite3_close(db); }

    void exec(const char* sql) { REQUIRE(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK); }

    std::int64_t scalar(const char* sql) {
        sqlite3_stmt* statement = nullptr;
        REQUIRE(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
        const auto result = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return result;
    }
    sqlite3* db = nullptr;
};
CheckRequest identified()
{
    return {*parse_uuid("12345678-1234-4234-8234-1234567890ab"), "launcher"};
}

std::string read(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

struct Calls {
    std::vector<std::string> events;
    bool fail_log = false;
    bool fail_record = false;
    bool fail_register = false;
};
class Log final : public ContactSink {
public:
    explicit Log(Calls& calls) : calls(calls) {}

    Result<void> append(const CheckRequest&, WallTime) override {
        calls.events.push_back("log");
        if (calls.fail_log) return std::unexpected("injected");
        return {};
    }
    Calls& calls;
};
class Census final : public CensusSink {
public:
    explicit Census(Calls& calls) : calls(calls) {}

    Result<void> register_suites(const Catalog&) override {
        calls.events.push_back("register");
        if (calls.fail_register) return std::unexpected("injected");
        return {};
    }

    Result<void> record(const CheckRequest&, WallTime) override {
        calls.events.push_back("commit");
        if (calls.fail_record) return std::unexpected("injected");
        return {};
    }

    Result<bool> prune(WallTime, const Retention&) override { return false; }
    Calls& calls;
};
}

TEST_CASE("Census migrates durable state and deduplicates installations and UTC days", "[census]")
{
    test::Directory directory;
    const auto path = directory.root / "census.sqlite";
    auto opened = SqliteCensus::open(path);
    REQUIRE(opened);
    REQUIRE((*opened)->register_suites(*test::catalog()));
    const auto at = test::published().seconds;
    auto request = identified();
    REQUIRE((*opened)->record(request, at));
    REQUIRE((*opened)->record(request, at + 1h));
    REQUIRE((*opened)->record(request, at - 1h));
    REQUIRE((*opened)->record(request, at + 24h));
    request.suite = "other";
    REQUIRE((*opened)->record(request, at));
    REQUIRE((*opened)->record({nil_uuid, "launcher"}, at));
    REQUIRE((*opened)->record({nil_uuid, "launcher"}, at));
    REQUIRE_FALSE((*opened)->record({nil_uuid, "missing"}, at));
    REQUIRE_FALSE((*opened)->record(request, WallTime{-1s}));
    opened->reset();
    REQUIRE(SqliteCensus::open(path));
    Database query(path);
    REQUIRE(query.scalar("PRAGMA user_version") == 1);
    REQUIRE(query.scalar("SELECT count(*) FROM installation") == 2);
    REQUIRE(query.scalar("SELECT count(*) FROM installation_day") == 3);
    REQUIRE(query.scalar("SELECT sum(request_count) FROM anonymous_day") == 2);
    REQUIRE(query.scalar("SELECT first_seen_s FROM installation WHERE suite_id='launcher'")
        == (at - 1h).time_since_epoch().count());
    REQUIRE(query.scalar("SELECT last_seen_s FROM installation WHERE suite_id='launcher'")
        == (at + 24h).time_since_epoch().count());
    REQUIRE(query.scalar("SELECT count(DISTINCT installation_id) FROM installation_day") == 1);
}

TEST_CASE("Census rolls back a failed day insert and bounds writer lock waits", "[census]")
{
    test::Directory directory;
    const auto path = directory.root / "census.sqlite";
    auto census = SqliteCensus::open(path, 10ms);
    REQUIRE(census);
    REQUIRE((*census)->register_suites(*test::catalog()));
    Database writer(path);
    writer.exec("CREATE TRIGGER reject_day BEFORE INSERT ON installation_day BEGIN SELECT RAISE(ABORT,'injected'); END;");
    REQUIRE_FALSE((*census)->record(identified(), test::published().seconds));
    REQUIRE(writer.scalar("SELECT count(*) FROM installation") == 0);
    writer.exec("DROP TRIGGER reject_day");
    writer.exec("BEGIN IMMEDIATE");
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_FALSE((*census)->record(identified(), test::published().seconds));
    REQUIRE(std::chrono::steady_clock::now() - start < 1s);
    writer.exec("ROLLBACK");
    REQUIRE((*census)->record(identified(), test::published().seconds));
}

TEST_CASE("Census refuses unknown or ambiguous schema versions", "[census]")
{
    test::Directory directory;
    const auto path = directory.root / "census.sqlite";
    {
        Database database(path);
        SECTION("unknown version") { database.exec("PRAGMA user_version=9"); }
        SECTION("unversioned data") { database.exec("CREATE TABLE unrelated(x)"); }
        SECTION("incomplete version one") { database.exec("PRAGMA user_version=1"); }
    }
    REQUIRE_FALSE(SqliteCensus::open(path));
}

TEST_CASE("Census pruning honors cutoff equality in bounded batches", "[census]")
{
    test::Directory directory;
    const auto path = directory.root / "census.sqlite";
    auto census = SqliteCensus::open(path);
    REQUIRE(census);
    REQUIRE((*census)->register_suites(*test::catalog()));
    const auto day = [](int value) { return WallTime{std::chrono::hours(value * 24)}; };
    auto old = identified();
    auto boundary = old;
    boundary.installation_id.back() = std::byte{0xac};
    auto current = old;
    current.installation_id.back() = std::byte{0xad};
    for (const auto value : {13, 14}) REQUIRE((*census)->record(old, day(value)));
    REQUIRE((*census)->record(boundary, day(15)));
    for (const auto value : {17, 18, 20}) REQUIRE((*census)->record(current, day(value)));
    for (const auto value : {17, 18}) REQUIRE((*census)->record({nil_uuid, "launcher"}, day(value)));
    bool complete = false;
    for (int n = 0; n < 20; ++n) {
        const auto pruned = (*census)->prune(day(20), {3, 5, 1});
        REQUIRE(pruned);
        if (!*pruned) { complete = true; break; }
    }
    REQUIRE(complete);
    Database query(path);
    REQUIRE(query.scalar("SELECT count(*) FROM installation") == 2);
    REQUIRE(query.scalar("SELECT count(*) FROM installation_day") == 2);
    REQUIRE(query.scalar("SELECT min(day_utc) FROM installation_day") == 18);
    REQUIRE(query.scalar("SELECT sum(request_count) FROM anonymous_day") == 1);
    query.exec("DELETE FROM installation");
    REQUIRE(query.scalar("SELECT count(*) FROM installation_day") == 0);
}

TEST_CASE("Inactive installations expire even when activity retention is longer", "[census]")
{
    test::Directory directory;
    const auto path = directory.root / "census.sqlite";
    auto census = SqliteCensus::open(path);
    REQUIRE(census);
    REQUIRE((*census)->register_suites(*test::catalog()));
    REQUIRE((*census)->record(identified(), WallTime{std::chrono::hours(14 * 24)}));
    for (int n = 0; n < 3; ++n)
        REQUIRE((*census)->prune(WallTime{std::chrono::hours(20 * 24)}, {10, 5, 1}));
    Database query(path);
    REQUIRE(query.scalar("SELECT count(*) FROM installation") == 0);
    REQUIRE(query.scalar("SELECT count(*) FROM installation_day") == 0);
}

TEST_CASE("Contact log rotates at the exact boundary and repairs an incomplete tail", "[log]")
{
    test::Directory directory;
    const auto at = test::published().seconds;
    const auto line = contact_line(identified(), at);
    REQUIRE(line);
    auto log = RollingContactLog::open(directory.root, 2 * std::ssize(*line));
    REQUIRE(log);
    REQUIRE((*log)->append(identified(), at));
    REQUIRE((*log)->append(identified(), at));
    REQUIRE(read(directory.root / "current.jsonl") == *line + *line);
    REQUIRE((*log)->append(identified(), at));
    REQUIRE(read(directory.root / "previous.jsonl") == *line + *line);
    log->reset();
    {
        std::ofstream append(directory.root / "current.jsonl", std::ios::app);
        append << "incomplete";
    }
    log = RollingContactLog::open(directory.root, 2 * std::ssize(*line));
    REQUIRE(log);
    REQUIRE(read(directory.root / "current.jsonl") == *line);
    REQUIRE((*log)->append(identified(), at));
    REQUIRE(read(directory.root / "current.jsonl") == *line + *line);
}

TEST_CASE("Contact log retries interrupted short writes and recovers partial failures", "[log]")
{
    test::Directory directory;
    int writes = 0;
    bool inject_failure = false;
    LogOperations operations;
    operations.write = [&](int fd, std::span<const std::byte> bytes) -> std::ptrdiff_t {
        ++writes;
        if (writes == 1) { errno = EINTR; return -1; }
        if (inject_failure && writes == 3) { errno = EIO; return -1; }
        return ::write(fd, bytes.data(), std::min<std::size_t>(7, bytes.size()));
    };
    SECTION("interrupted and short") {}
    SECTION("partial failure") { inject_failure = true; }
    auto log = RollingContactLog::open(directory.root, 4096, operations);
    REQUIRE(log);
    const auto first = (*log)->append(identified(), test::published().seconds);
    if (inject_failure) {
        REQUIRE_FALSE(first);
        inject_failure = false;
        REQUIRE((*log)->append(identified(), test::published().seconds));
    } else REQUIRE(first);
    REQUIRE(read(directory.root / "current.jsonl") == *contact_line(identified(), test::published().seconds));
}

TEST_CASE("Contact log reports sync and rotation failures without losing bounds", "[log]")
{
    test::Directory directory;
    bool fail_sync = false;
    bool fail_rotate = false;
    LogOperations operations;
    operations.sync = [&](int fd) {
        if (fail_sync) { errno = EIO; return -1; }
        return ::fsync(fd);
    };
    operations.rotate = [&](int fd) {
        if (fail_rotate) { errno = EACCES; return -1; }
        return ::renameat(fd, "current.jsonl", fd, "previous.jsonl");
    };
    const auto at = test::published().seconds;
    const auto line = *contact_line(identified(), at);
    auto log = RollingContactLog::open(directory.root, std::ssize(line), operations);
    REQUIRE(log);
    SECTION("sync") { fail_sync = true; }
    SECTION("rotation") {
        REQUIRE((*log)->append(identified(), at));
        fail_rotate = true;
    }
    REQUIRE_FALSE((*log)->append(identified(), at));
    fail_sync = fail_rotate = false;
    REQUIRE((*log)->append(identified(), at));
    REQUIRE(std::filesystem::file_size(directory.root / "current.jsonl") <= line.size());
    if (std::filesystem::exists(directory.root / "previous.jsonl"))
        REQUIRE(std::filesystem::file_size(directory.root / "previous.jsonl") <= line.size());
}

TEST_CASE("Contact log rejects symlinks oversized files and unsafe contacts", "[log]")
{
    test::Directory directory;
    SECTION("symbolic current") {
        directory.write("target", "");
        std::filesystem::create_symlink(directory.root / "target", directory.root / "current.jsonl");
        REQUIRE_FALSE(RollingContactLog::open(directory.root));
    }
    SECTION("oversized previous") {
        directory.write("previous.jsonl", std::string(100, 'x'));
        REQUIRE_FALSE(RollingContactLog::open(directory.root, 10));
    }
    REQUIRE_FALSE(contact_line({nil_uuid, "bad\"suite"}, test::published().seconds));
}

TEST_CASE("Processor preserves persistence ordering and cancellation semantics", "[processor]")
{
    Calls calls;
    auto catalog = test::catalog();
    SECTION("success") {}
    SECTION("log failure") { calls.fail_log = true; }
    SECTION("SQL failure") { calls.fail_record = true; }
    SECTION("render failure") { catalog->suites.at("launcher").title = "\x01"; }
    CheckProcessor processor(std::move(catalog), {}, std::make_unique<Log>(calls), std::make_unique<Census>(calls));
    REQUIRE(processor.initialize());
    calls.events.clear();
    const auto reply = processor.process(identified(), {1, test::published().seconds}, [] { return true; });
    if (reply.category == "feed") REQUIRE(calls.events.empty());
    else if (calls.fail_log) REQUIRE(calls.events == std::vector<std::string>{"log"});
    else REQUIRE(calls.events == std::vector<std::string>{"log", "commit"});
    REQUIRE((reply.outcome == Outcome::ok) == (!calls.fail_log && !calls.fail_record && reply.category.empty()));
    calls.events.clear();
    REQUIRE(processor.process(identified(), {2, test::published().seconds}, [] { return false; }).outcome == Outcome::unavailable);
    REQUIRE(calls.events.empty());
    REQUIRE(processor.process({nil_uuid, "missing"}, {}, [] { return true; }).outcome == Outcome::not_found);
    REQUIRE(calls.events.empty());
}

TEST_CASE("Processor activates a catalog only after suite registration commits", "[processor]")
{
    Calls calls;
    CheckProcessor processor(test::catalog(), {}, std::make_unique<Log>(calls), std::make_unique<Census>(calls));
    REQUIRE(processor.initialize());
    calls.fail_register = true;
    REQUIRE_FALSE(processor.activate(test::catalog("rejected")));
    REQUIRE(processor.revision() == "sample-001");
    calls.fail_register = false;
    REQUIRE(processor.activate(test::catalog("accepted")));
    REQUIRE(processor.revision() == "accepted");
}

TEST_CASE("Catalog activation preserves revisions build identities and artifact bytes", "[processor]")
{
    Calls calls;
    CheckProcessor processor(test::catalog(), {}, std::make_unique<Log>(calls), std::make_unique<Census>(calls));
    REQUIRE(processor.initialize());
    auto candidate = test::catalog("next");
    SECTION("revision reuse") {
        candidate->header.revision = "sample-001";
        candidate->packages.at("app").title = "Changed";
    }
    SECTION("version rewrite") { candidate->builds.begin()->second.version = "changed"; }
    SECTION("artifact rewrite") { ++candidate->builds.begin()->second.artifacts.begin()->second.size; }
    REQUIRE_FALSE(processor.activate(std::move(candidate)));
    REQUIRE(processor.revision() == "sample-001");
}
