#include "updater/persistence.hpp"

#include <sqlite3.h>

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#endif

namespace updater {
namespace {

struct SqlError : std::runtime_error { using std::runtime_error::runtime_error; };
void check(sqlite3* db, int code)
{
    if (code != SQLITE_OK) throw SqlError(sqlite3_errmsg(db));
}

void exec(sqlite3* db, const char* sql) { check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db)
    {
        check(db, sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr));
    }

    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void text(int index, std::string_view value) {
        check(db_, sqlite3_bind_text64(statement_, index, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
    }

    void uuid(int index, const Uuid& value) {
        check(db_, sqlite3_bind_blob(statement_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }

    void integer(int index, std::int64_t value) { check(db_, sqlite3_bind_int64(statement_, index, value)); }

    bool row() {
        const auto code = sqlite3_step(statement_);
        if (code == SQLITE_ROW) return true;
        if (code != SQLITE_DONE) throw SqlError(sqlite3_errmsg(db_));
        return false;
    }

    void run() { if (row()) throw SqlError("unexpected SQL row"); }

    std::int64_t integer(int column) const { return sqlite3_column_int64(statement_, column); }

    std::string text(int column) const {
        const auto* value = sqlite3_column_text(statement_, column);
        return value ? reinterpret_cast<const char*>(value) : "";
    }
private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { exec(db_, "BEGIN IMMEDIATE"); }

    ~Transaction() { if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() { exec(db_, "COMMIT"); committed_ = true; }
private:
    sqlite3* db_;
    bool committed_ = false;
};

constexpr auto migration = R"(
CREATE TABLE suite (
    suite_id TEXT PRIMARY KEY,
    CHECK(length(suite_id) BETWEEN 1 AND 64),
    CHECK(substr(suite_id,1,1) GLOB '[a-z]'),
    CHECK(suite_id NOT GLOB '*[^a-z0-9_-]*')
) STRICT, WITHOUT ROWID;
CREATE TABLE installation (
    suite_id TEXT NOT NULL REFERENCES suite(suite_id),
    installation_id BLOB NOT NULL,
    first_seen_s INTEGER NOT NULL CHECK(first_seen_s >= 0),
    last_seen_s INTEGER NOT NULL CHECK(last_seen_s >= first_seen_s),
    PRIMARY KEY(suite_id,installation_id),
    CHECK(length(installation_id)=16),
    CHECK(installation_id <> zeroblob(16))
) STRICT, WITHOUT ROWID;
CREATE TABLE installation_day (
    suite_id TEXT NOT NULL,
    installation_id BLOB NOT NULL,
    day_utc INTEGER NOT NULL CHECK(day_utc >= 0),
    PRIMARY KEY(suite_id,installation_id,day_utc),
    FOREIGN KEY(suite_id,installation_id) REFERENCES installation(suite_id,installation_id) ON DELETE CASCADE
) STRICT, WITHOUT ROWID;
CREATE TABLE anonymous_day (
    suite_id TEXT NOT NULL REFERENCES suite(suite_id),
    day_utc INTEGER NOT NULL CHECK(day_utc >= 0),
    request_count INTEGER NOT NULL CHECK(request_count > 0),
    PRIMARY KEY(suite_id,day_utc)
) STRICT, WITHOUT ROWID;
CREATE INDEX installation_by_last_contact ON installation(last_seen_s,suite_id,installation_id);
CREATE INDEX activity_by_day ON installation_day(day_utc,suite_id,installation_id);
PRAGMA user_version=1;
)";

} // namespace

struct SqliteCensus::Impl {
    sqlite3* db = nullptr;
    ~Impl() { if (db) sqlite3_close_v2(db); }
};

SqliteCensus::SqliteCensus(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SqliteCensus::~SqliteCensus() = default;

Result<std::unique_ptr<SqliteCensus>> SqliteCensus::open(
    const std::filesystem::path& path, std::chrono::milliseconds busy_timeout)
{
    if (busy_timeout.count() < 0 || !std::in_range<int>(busy_timeout.count()))
        return std::unexpected("invalid SQLite busy timeout");
    auto impl = std::make_unique<Impl>();
    try {
        const auto opened = sqlite3_open_v2(path.c_str(), &impl->db,
                                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
        check(impl->db, opened);
        auto* db = impl->db;
        check(db, sqlite3_busy_timeout(db, static_cast<int>(busy_timeout.count())));
        exec(db, "PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL; PRAGMA journal_mode=DELETE;");
        std::int64_t version;
        { Statement query(db, "PRAGMA user_version"); query.row(); version = query.integer(0); }
        if (version == 0) {
            {
                Statement query(db, "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'");
                query.row();
                if (query.integer(0) != 0) throw SqlError("refusing migration of nonempty unversioned database");
            }
            Transaction transaction(db);
            exec(db, migration);
            transaction.commit();
        } else if (version != 1) {
            throw SqlError("unsupported census schema version");
        }
        {
            Statement integrity(db, "PRAGMA quick_check");
            if (!integrity.row() || integrity.text(0) != "ok") throw SqlError("census integrity check failed");
            Statement foreign(db, "PRAGMA foreign_key_check");
            if (foreign.row()) throw SqlError("census foreign key check failed");
        }
        for (const auto sql : {
            "SELECT suite_id FROM suite LIMIT 0",
            "SELECT suite_id,installation_id,first_seen_s,last_seen_s FROM installation LIMIT 0",
            "SELECT suite_id,installation_id,day_utc FROM installation_day LIMIT 0",
            "SELECT suite_id,day_utc,request_count FROM anonymous_day LIMIT 0"}) {
            Statement validate(db, sql);
        }
        return std::unique_ptr<SqliteCensus>(new SqliteCensus(std::move(impl)));
    } catch (const SqlError& error) { return std::unexpected(error.what()); }
}

Result<void> SqliteCensus::register_suites(const Catalog& catalog)
{
    try {
        Transaction transaction(impl_->db);
        for (const auto& id : catalog.suites.keys()) {
            Statement statement(impl_->db, "INSERT INTO suite(suite_id) VALUES(?) ON CONFLICT DO NOTHING");
            statement.text(1, id);
            statement.run();
        }
        transaction.commit();
        return {};
    } catch (const SqlError& error) { return std::unexpected(error.what()); }
}

Result<void> SqliteCensus::record(const CheckRequest& request, WallTime observed_at)
{
    const auto seconds = observed_at.time_since_epoch().count();
    if (seconds < 0) return std::unexpected("negative census timestamp");
    const auto utc_day = std::chrono::floor<std::chrono::days>(observed_at).time_since_epoch().count();
    try {
        Transaction transaction(impl_->db);
        if (request.installation_id == nil_uuid) {
            Statement count(impl_->db, R"(
INSERT INTO anonymous_day(suite_id,day_utc,request_count) VALUES(?,?,1)
ON CONFLICT(suite_id,day_utc) DO UPDATE SET request_count=anonymous_day.request_count+1)");
            count.text(1, request.suite);
            count.integer(2, utc_day);
            count.run();
        } else {
            Statement installation(impl_->db, R"(
INSERT INTO installation(suite_id,installation_id,first_seen_s,last_seen_s) VALUES(?,?,?,?)
ON CONFLICT(suite_id,installation_id) DO UPDATE SET
 first_seen_s=min(installation.first_seen_s,excluded.first_seen_s),
 last_seen_s=max(installation.last_seen_s,excluded.last_seen_s))");
            installation.text(1, request.suite);
            installation.uuid(2, request.installation_id);
            installation.integer(3, seconds);
            installation.integer(4, seconds);
            installation.run();
            Statement day(impl_->db, R"(
INSERT INTO installation_day(suite_id,installation_id,day_utc) VALUES(?,?,?) ON CONFLICT DO NOTHING)");
            day.text(1, request.suite);
            day.uuid(2, request.installation_id);
            day.integer(3, utc_day);
            day.run();
        }
        transaction.commit();
        return {};
    } catch (const SqlError& error) { return std::unexpected(error.what()); }
}

Result<bool> SqliteCensus::prune(WallTime now, const Retention& retention)
{
    const auto seconds = now.time_since_epoch().count();
    if (seconds < 0 || retention.activity_days <= 0 || retention.inactive_days <= 0
        || retention.batch <= 0 || retention.activity_days > std::numeric_limits<std::int64_t>::max() / 86400
        || retention.inactive_days > std::numeric_limits<std::int64_t>::max() / 86400)
        return std::unexpected("invalid retention policy");
    const auto keep = std::max<std::int64_t>(0, seconds / 86400 - retention.activity_days + 1);
    const auto inactive = std::max<std::int64_t>(0, seconds - retention.inactive_days * 86400);
    try {
        Transaction transaction(impl_->db);
        bool more = false;
        for (const auto sql : {
            R"(DELETE FROM installation_day WHERE (suite_id,installation_id,day_utc) IN (
SELECT suite_id,installation_id,day_utc FROM installation_day WHERE day_utc < ? ORDER BY day_utc,suite_id,installation_id LIMIT ?))",
            R"(DELETE FROM anonymous_day WHERE (suite_id,day_utc) IN (
SELECT suite_id,day_utc FROM anonymous_day WHERE day_utc < ? ORDER BY day_utc,suite_id LIMIT ?))"}) {
            Statement prune(impl_->db, sql);
            prune.integer(1, keep);
            prune.integer(2, retention.batch);
            prune.run();
            more |= sqlite3_changes64(impl_->db) == retention.batch;
        }
        // Remove an inactive pair's remaining days in a bounded batch even when
        // activity retention is longer than the installation inactivity policy.
        Statement inactive_days(impl_->db, R"(
DELETE FROM installation_day WHERE (suite_id,installation_id,day_utc) IN (
 SELECT d.suite_id,d.installation_id,d.day_utc FROM installation AS i
 JOIN installation_day AS d ON d.suite_id=i.suite_id AND d.installation_id=i.installation_id
 WHERE i.last_seen_s < ? ORDER BY i.last_seen_s,i.suite_id,i.installation_id,d.day_utc LIMIT ?))");
        inactive_days.integer(1, inactive);
        inactive_days.integer(2, retention.batch);
        inactive_days.run();
        more |= sqlite3_changes64(impl_->db) == retention.batch;
        Statement installations(impl_->db, R"(
DELETE FROM installation WHERE (suite_id,installation_id) IN (
 SELECT i.suite_id,i.installation_id FROM installation AS i WHERE last_seen_s < ?
 AND NOT EXISTS (SELECT 1 FROM installation_day AS d WHERE d.suite_id=i.suite_id AND d.installation_id=i.installation_id)
 ORDER BY last_seen_s,suite_id,installation_id LIMIT ?))");
        installations.integer(1, inactive);
        installations.integer(2, retention.batch);
        installations.run();
        more |= sqlite3_changes64(impl_->db) == retention.batch;
        transaction.commit();
        return more;
    } catch (const SqlError& error) { return std::unexpected(error.what()); }
}

} // namespace updater
