# Census database design

This is the implemented SQLite schema for the [server design](DESIGN.md).
With a nonzero installation UUID, the census stores the UUID and suite, first
and last contact times, and one activity row per observed UTC day. A UUID can
link repeat observations. A Nil check increments an anonymous request count
by suite and UTC day, with no installation row.

Neither the database nor the contact log includes the IP address, package,
track, platform, or installed version. The rolling contact log uses its own
size-based retention and includes repeat attempts. Reports use the committed
database. See [daemon settings and recorded data](docs/settings.md).

## Schema version 1

Open the database on the guest's native filesystem. Use one writer connection owned by the worker and a separate snapshot for reporting. Enable foreign keys on every connection that mutates state. Start with SQLite's rollback journal and full synchronization. This design has one writer and does not need concurrent live reporting. Bound lock waits to 2 seconds and map temporary write failures to HTTP 503. Use prepared statements with bound parameters.

Timestamps are nonnegative Unix seconds in UTC. The day number is the integer UTC day since 1970-01-01, computed from the same admission timestamp. This is independent of the Windows client's last-check day encoding. Preserve the normalized UUID as a 16-byte BLOB in network byte order. Zero UUID bytes are reserved for the anonymous path.

Apply this migration transaction to a version-zero database only, after confirming it is empty. Enable foreign keys before entering the transaction. Fail startup for unknown schema versions. Record later migrations in source, back up before applying them, and test opening old fixtures before deployment.

```sql
PRAGMA foreign_keys = ON;
BEGIN IMMEDIATE;

CREATE TABLE suite (
    suite_id TEXT PRIMARY KEY,
    CHECK (length(suite_id) BETWEEN 1 AND 64),
    CHECK (substr(suite_id, 1, 1) GLOB '[a-z]'),
    CHECK (suite_id NOT GLOB '*[^a-z0-9_-]*')
) STRICT, WITHOUT ROWID;

CREATE TABLE installation (
    suite_id TEXT NOT NULL REFERENCES suite(suite_id),
    installation_id BLOB NOT NULL,
    first_seen_s INTEGER NOT NULL CHECK (first_seen_s >= 0),
    last_seen_s INTEGER NOT NULL CHECK (last_seen_s >= first_seen_s),
    PRIMARY KEY (suite_id, installation_id),
    CHECK (length(installation_id) = 16),
    CHECK (installation_id <> zeroblob(16))
) STRICT, WITHOUT ROWID;

CREATE TABLE installation_day (
    suite_id TEXT NOT NULL,
    installation_id BLOB NOT NULL,
    day_utc INTEGER NOT NULL CHECK (day_utc >= 0),
    PRIMARY KEY (suite_id, installation_id, day_utc),
    FOREIGN KEY (suite_id, installation_id)
        REFERENCES installation(suite_id, installation_id) ON DELETE CASCADE
) STRICT, WITHOUT ROWID;

CREATE TABLE anonymous_day (
    suite_id TEXT NOT NULL REFERENCES suite(suite_id),
    day_utc INTEGER NOT NULL CHECK (day_utc >= 0),
    request_count INTEGER NOT NULL CHECK (request_count > 0),
    PRIMARY KEY (suite_id, day_utc)
) STRICT, WITHOUT ROWID;

CREATE INDEX installation_by_last_contact
    ON installation(last_seen_s, suite_id, installation_id);

CREATE INDEX activity_by_day
    ON installation_day(day_utc, suite_id, installation_id);

PRAGMA user_version = 1;
COMMIT;
```

The catalog owns suite names, routing, and package metadata. Insert the canonical IDs of the validated catalog's suites before activating that snapshot. Keep previously seen suite IDs so a suite removed from the feed remains reportable. Unknown incoming suite names never create database records. Treat a canonical ID rename as a deliberate data migration.

## Recording a real installation

Run both statements in one transaction. The worker has already resolved the suite and appended the contact line as described in the server design. The first and last timestamps are the extrema of committed observations, so an out-of-order observation does not regress the last contact. A repeat on the same day adds no activity row. Requests observed on different UTC days count on their respective days.

```sql
INSERT INTO installation (suite_id, installation_id, first_seen_s, last_seen_s)
VALUES (:suite, :uuid, :seen_s, :seen_s)
ON CONFLICT (suite_id, installation_id) DO UPDATE SET
    first_seen_s = min(installation.first_seen_s, excluded.first_seen_s),
    last_seen_s = max(installation.last_seen_s, excluded.last_seen_s);

INSERT INTO installation_day (suite_id, installation_id, day_utc)
VALUES (:suite, :uuid, :day_utc)
ON CONFLICT (suite_id, installation_id, day_utc) DO NOTHING;
```

Use a transaction guard with rollback on unsuccessful completion. Return success only after commit. A retry following a lost response can move last contact forward and produce an additional rolling-log line. It cannot create a second installation or a second activity row for the same day. This is safe retry behavior for distinct measures rather than an exactly-once request guarantee. [SQLite UPSERT](https://sqlite.org/lang_upsert.html)

## Recording a Nil contact

Run this statement in its own transaction. Nil traffic has no installation row, first or last contact, or retention cohort. An increment measures one committed request, which can include a retry.

```sql
INSERT INTO anonymous_day (suite_id, day_utc, request_count)
VALUES (:suite, :day_utc, 1)
ON CONFLICT (suite_id, day_utc) DO UPDATE SET
    request_count = anonymous_day.request_count + 1;
```

## Reports and interpretation

The implemented [census tool](tools/census.py) creates a consistent temporary
SQLite snapshot before running report queries. It emits JSON with the selected
UTC window and per-suite measures. Cohort reports require the actual configured
retention bounds and reject incomplete return windows. The same tool creates
verified backups without overwriting existing destinations.

| Report | Definition |
| --- | --- |
| Newly observed installations | Retained installation rows with first contact inside the selected period, grouped by suite |
| Distinct active installations | Distinct suite/UUID pairs with a day in the selected period. Present 30-, 60-, or 90-day windows explicitly rather than labeling an installation permanently inactive. |
| Returned cohort | Installations first observed in the cohort interval with any observation in a later, disjoint return interval. Only report cohorts whose return window has elapsed and is within retained activity history. |
| Recurrence | Number of distinct UTC days or calendar months observed within a stated window. The client cadence is unknown and varies. |
| Anonymous traffic | Sum of committed Nil request counts for the stated period, shown separately from distinct installations |

The measurements describe observed installations. Missing contacts can result from scheduling, connectivity, user choice, or stopped use. A long gap does not establish an uninstall. A UUID used by several suites is counted within each suite. A cross-suite unique count, if ever needed, must deduplicate those UUIDs deliberately.

Use half-open intervals and compute UTC boundaries outside SQL. These are initial report queries for the test suite and operator tooling.

```sql
-- Count each pair once in the selected activity window.
SELECT suite_id, count(DISTINCT installation_id) AS active_installations
FROM installation_day
WHERE day_utc >= :start_day AND day_utc < :end_day
GROUP BY suite_id
ORDER BY suite_id;

-- The caller supplies a later, disjoint UTC return window.
SELECT i.suite_id, count(*) AS cohort_size,
       sum(EXISTS (
           SELECT 1 FROM installation_day AS d
           WHERE d.suite_id = i.suite_id
             AND d.installation_id = i.installation_id
             AND d.day_utc >= :return_start_day
             AND d.day_utc < :return_end_day
       )) AS returned
FROM installation AS i
WHERE i.first_seen_s >= :cohort_start_s AND i.first_seen_s < :cohort_end_s
GROUP BY i.suite_id
ORDER BY i.suite_id;

SELECT suite_id, sum(request_count) AS anonymous_requests
FROM anonymous_day
WHERE day_utc >= :start_day AND day_utc < :end_day
GROUP BY suite_id
ORDER BY suite_id;
```

## Retention, maintenance, and recovery

The defaults keep 400 UTC day buckets including today and remove pairs whose last contact is older than 730 days. Pruning removes any remaining activity for an inactive pair in bounded batches before deleting the pair. A later contact becomes newly observed again. Report retention bounds with the results, and use backups or separately planned aggregate reports for comparisons outside the retained window. Anonymous request counts and activity associated with an installation UUID use the same day-bucket window. The bounded raw log rotates by size independently.

Run pruning in small worker-owned transactions between check-ins, once daily and after startup if needed. Use a UTC day cutoff supplied by the clock policy. Resume unfinished batches later. A maintenance error is visible in the service journal and leaves normal check-in work able to proceed if its own persistence succeeds. Never interpret an older observed clock value as an instruction to move timestamps backwards.

```sql
-- Repeat bounded batches until the relevant cutoff is satisfied.
DELETE FROM installation_day
WHERE (suite_id, installation_id, day_utc) IN (
    SELECT suite_id, installation_id, day_utc
    FROM installation_day WHERE day_utc < :keep_from_day
    ORDER BY day_utc, suite_id, installation_id LIMIT :batch
);

DELETE FROM anonymous_day
WHERE (suite_id, day_utc) IN (
    SELECT suite_id, day_utc
    FROM anonymous_day WHERE day_utc < :keep_from_day
    ORDER BY day_utc, suite_id LIMIT :batch
);

-- Remove remaining days of inactive pairs in a separate bounded batch.
-- This also handles an activity window longer than the inactivity cutoff.
DELETE FROM installation_day
WHERE (suite_id, installation_id, day_utc) IN (
    SELECT d.suite_id, d.installation_id, d.day_utc
    FROM installation AS i JOIN installation_day AS d
      ON d.suite_id = i.suite_id AND d.installation_id = i.installation_id
    WHERE i.last_seen_s < :inactive_before_s
    ORDER BY i.last_seen_s, i.suite_id, i.installation_id, d.day_utc LIMIT :batch
);

DELETE FROM installation
WHERE (suite_id, installation_id) IN (
    SELECT i.suite_id, i.installation_id
    FROM installation AS i WHERE i.last_seen_s < :inactive_before_s
      AND NOT EXISTS (
          SELECT 1 FROM installation_day AS d
          WHERE d.suite_id = i.suite_id AND d.installation_id = i.installation_id
      )
    ORDER BY last_seen_s, suite_id, installation_id LIMIT :batch
);
```

The census tool can create a consistent backup with SQLite's backup API and
check its integrity and foreign keys. Backups preserve the census data and
are independent of the daemon's pruning. The rolling contact log holds only
recent contacts and cannot reconstruct the full census. [SQLite backup API](https://sqlite.org/backup.html)
