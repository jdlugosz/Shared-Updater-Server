#!/usr/bin/env python3
"""Consistent census backups and reports from a private local snapshot."""
import argparse
from contextlib import closing
import datetime as dt
import json
import os
from pathlib import Path
import sqlite3
import tempfile
import time


def readonly(path):
    connection = sqlite3.connect(Path(path).resolve().as_uri() + "?mode=ro", uri=True, timeout=2)
    if connection.execute("PRAGMA user_version").fetchone()[0] != 1:
        connection.close()
        raise ValueError("unsupported census schema version")
    return connection


def verify(connection):
    if connection.execute("PRAGMA quick_check").fetchall() != [("ok",)]:
        raise ValueError("database integrity check failed")
    if connection.execute("PRAGMA foreign_key_check").fetchall():
        raise ValueError("database foreign key check failed")


def copy_database(source, destination, timeout=30):
    deadline = time.monotonic() + timeout
    def progress(status, remaining, total):
        if time.monotonic() >= deadline:
            raise TimeoutError("census snapshot deadline exceeded")
    source.backup(destination, pages=128, progress=progress, sleep=0.02)
    verify(destination)


def backup(source_path, destination_path):
    destination_path = Path(destination_path)
    parent = destination_path.parent.resolve()
    status = parent.stat()
    if status.st_uid != os.geteuid() or status.st_mode & 0o077:
        raise ValueError("backup directory must be private and owned by the current user")
    fd = os.open(destination_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    os.close(fd)
    try:
        with closing(readonly(source_path)) as source, closing(sqlite3.connect(destination_path)) as destination:
            copy_database(source, destination)
        fd = os.open(destination_path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
        fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except BaseException:
        destination_path.unlink(missing_ok=True)
        raise


def report(connection, start_day, end_day):
    if start_day < 0 or end_day <= start_day:
        raise ValueError("invalid half-open UTC day window")
    result = {"schema_version": 1, "start_day": start_day, "end_day": end_day, "suites": []}
    for (suite,) in connection.execute("SELECT suite_id FROM suite ORDER BY suite_id"):
        retained = connection.execute("SELECT count(*) FROM installation WHERE suite_id=?", (suite,)).fetchone()[0]
        active = connection.execute("""
SELECT count(DISTINCT installation_id) FROM installation_day
WHERE suite_id=? AND day_utc>=? AND day_utc<?""", (suite, start_day, end_day)).fetchone()[0]
        new = connection.execute("""
SELECT count(*) FROM installation WHERE suite_id=? AND first_seen_s>=? AND first_seen_s<?""",
                                 (suite, start_day * 86400, end_day * 86400)).fetchone()[0]
        anonymous = connection.execute("""
SELECT coalesce(sum(request_count),0) FROM anonymous_day WHERE suite_id=? AND day_utc>=? AND day_utc<?""",
                                       (suite, start_day, end_day)).fetchone()[0]
        months = dict(connection.execute("""
SELECT strftime('%Y-%m',day_utc*86400,'unixepoch'), count(DISTINCT installation_id)
FROM installation_day WHERE suite_id=? AND day_utc>=? AND day_utc<?
GROUP BY 1 ORDER BY 1""", (suite, start_day, end_day)))
        result["suites"].append({"suite": suite, "retained_installations": retained,
            "newly_observed": new, "active_installations": active,
            "anonymous_requests": anonymous, "monthly_active": months})
    return result


def cohort(connection, start, end, return_start, return_end):
    if start < 0 or start >= end or return_start < end or return_start >= return_end:
        raise ValueError("cohort and return windows must be nonempty and disjoint")
    rows = connection.execute("""
SELECT i.suite_id,count(*),sum(EXISTS (
 SELECT 1 FROM installation_day AS d WHERE d.suite_id=i.suite_id AND d.installation_id=i.installation_id
 AND d.day_utc>=? AND d.day_utc<?))
FROM installation AS i WHERE i.first_seen_s>=? AND i.first_seen_s<?
GROUP BY i.suite_id ORDER BY i.suite_id""", (return_start, return_end, start * 86400, end * 86400))
    return [{"suite": suite, "cohort_size": size, "returned": returned} for suite, size, returned in rows]


def day(text):
    return (dt.date.fromisoformat(text) - dt.date(1970, 1, 1)).days


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    command = sub.add_parser("backup")
    command.add_argument("source")
    command.add_argument("destination")
    command = sub.add_parser("report")
    command.add_argument("database")
    command.add_argument("--days", type=int, default=30)
    command.add_argument("--end", help="Exclusive UTC end date; default tomorrow")
    command.add_argument("--cohort", nargs=4, metavar=("START", "END", "RETURN_START", "RETURN_END"))
    command.add_argument("--activity-days", type=int, help="Actual configured retention, required with --cohort")
    command.add_argument("--inactive-days", type=int, help="Actual configured retention, required with --cohort")
    command = sub.add_parser("check")
    command.add_argument("database")
    args = parser.parse_args()
    if args.operation == "backup":
        backup(args.source, args.destination)
        print("Backup verified.")
        return
    # Queries run on a disk snapshot, so reporting holds no long live-database read transaction.
    with tempfile.TemporaryDirectory(prefix="updater-census-report-") as temporary:
        with closing(readonly(args.database)) as source, closing(sqlite3.connect(Path(temporary) / "snapshot.sqlite")) as snapshot:
            copy_database(source, snapshot)
            if args.operation == "check":
                print("Database verified.")
                return
            today = day(dt.datetime.now(dt.timezone.utc).date().isoformat())
            end = day(args.end) if args.end else today + 1
            if args.days <= 0 or end > today + 1:
                raise ValueError("invalid reporting window")
            result = report(snapshot, end - args.days, end)
            if args.cohort:
                if not args.activity_days or not args.inactive_days or min(args.activity_days, args.inactive_days) <= 0:
                    raise ValueError("supply actual --activity-days and --inactive-days for cohort bounds")
                values = [day(value) for value in args.cohort]
                if values[0] < today - args.inactive_days + 1 or values[2] < today - args.activity_days + 1:
                    raise ValueError("cohort or return window falls outside configured retention")
                if values[3] > today:
                    raise ValueError("return window has not elapsed")
                result["cohort"] = cohort(snapshot, *values)
            print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, sqlite3.Error, TimeoutError) as error:
        raise SystemExit(f"Error: {error}")
