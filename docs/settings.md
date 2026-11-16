# Daemon settings and recorded data

## Configuration file

The daemon reads TOML from `/etc/shared-updater-server/server.toml` by default.
`--config FILE` selects another file. Relative paths in the settings resolve
from that file's directory. The parser rejects unknown keys and configuration
files larger than 1 MiB. See the [example settings](../examples/server.toml).

| Setting | Default | What it controls |
| --- | --- | --- |
| `listen` | `127.0.0.1:8080` | Backend HTTP address and port. The daemon accepts only a numeric loopback address, including bracketed IPv6 such as `[::1]:8080`. |
| `catalog_dir` | Required choice | Load the directory's top-level regular `.toml` fragments in filename order. |
| `catalog_files` | Required choice | Load a nonempty list of fragment paths. Specify exactly one of `catalog_dir` and `catalog_files`. |
| `publisher_id_base` | `https://dlugosz.enterprises/updater/id/` | Base for stable Atom IDs. It must be an absolute HTTPS URL ending in `/`, with no query or fragment. |
| `state_dir` | `/var/lib/shared-updater-server` | Directory containing `census.sqlite`. |
| `log_dir` | `/var/log/shared-updater-server` | Directory containing `current.jsonl` and, after rotation, `previous.jsonl`. |

The daemon opens its database and contact log when starting the service. It
locks the state and log directories to protect them from other daemon
instances. A live reload replaces the catalog. Changes to daemon settings
take effect on restart.

`--check-config` validates the configuration, catalog, and feed limits and
reports catalog usage. In this mode, `--catalog-dir DIRECTORY` can substitute a
candidate directory. `--feed SUITE [--id UUID]` prints one Atom feed to stdout,
using the all-zero UUID by default. Both modes exit without opening the HTTP
listener or writing census and contact-log records.

## Limits

Settings in the `[limits]` table are positive integers.

| Key | Default | What it controls |
| --- | ---: | --- |
| `connections` | 128 | Open backend HTTP connections, including those still sending headers. |
| `unfinished_requests` | 64 | Admitted check-ins across the queue, the worker, and pending responses. |
| `contact_log_bytes_per_file` | 1,048,576 | Cap on each of the two rolling contact-log files. It must fit a complete record for every configured suite. |
| `catalog_bytes` | 8,388,608 | Total bytes in the catalog fragments. |
| `catalog_fragments` | 4,096 | Number of catalog fragments. |
| `response_bytes` | 524,288 | Maximum Atom response size in bytes. The Client applies its own limit. |
| `header_bytes` | 8,192 | HTTP header limit in bytes. The minimum is 128. |
| `target_bytes` | 256 | Limit for the HTTP path and query together, from 1 through 256 bytes. |

## Timeouts

Settings in `[timeouts]` are in milliseconds and accept values from 1 through
600,000.

| Key | Default | What it controls |
| --- | ---: | --- |
| `header_ms` | 5,000 | Time allowed to read a request's headers. |
| `queue_ms` | 5,000 | Time a check-in may wait in the worker queue. |
| `write_ms` | 10,000 | Time allowed to write the HTTP response. |
| `sqlite_busy_ms` | 2,000 | How long SQLite waits for a database lock. |
| `worker_ms` | 10,000 | Worker progress budget used for health and watchdog decisions. |
| `reload_ms` | 30,000 | Deadline for the loader to validate a replacement catalog before it can be offered to the worker for activation. |
| `shutdown_ms` | 20,000 | Time budget for graceful service shutdown. |

## Census and contact log

Only a valid check-in for a known suite can reach the recorder. The daemon
prepares the feed, appends a contact record, commits the SQLite observation,
then returns success. A database error can leave a contact record even when
no observation was committed. Retries can add contact records and anonymous
request counts. They do not add a second daily activity row for the same suite
and nonzero UUID.

| Record | Stored data |
| --- | --- |
| Anonymous SQLite census | The all-zero Nil UUID increments a daily count for the suite. The database keeps the suite, UTC day, and request count. It creates no installation row. The count measures requests, including retries. |
| SQLite census with an installation UUID | The database stores the full nonzero UUID with the suite, the first and last observed UTC timestamps, and one activity row per observed UTC day for that pair. The UUID is a pseudonym that can link observations across days. |
| Rolling JSON Lines contact log | Each entry contains `at` (UTC timestamp), `id` (the supplied UUID, including Nil), and `suite`. The daemon keeps the current file and one previous file. Rotation is by size. |

The daemon does not put IP addresses, account names, raw requests, selected
platforms, or installed versions in these records. The health endpoint and
offline modes do not record check-ins. The settings have no switch that
disables the census or contact log. A client can use Nil to make an anonymous
check, which still contributes to the daily count and contact log.

A reverse proxy and the download host have their own logging behavior. The
[Caddy example](../deploy/updater.caddy) skips access logs for check-ins and
filters the request object from its check-in proxy error records.

## Retention

SQLite retention is applied in bounded maintenance batches. The contact log
rotates separately and retains at most two files, each capped at 1 MiB by
default.

| `[retention]` key | Default | What it controls |
| --- | ---: | --- |
| `activity_days` | 400 | UTC day buckets kept, including today, for both anonymous request counts and installation activity. |
| `inactive_installation_days` | 730 | Remove a suite/UUID pair and any remaining activity once its last contact is older than this many days. |
| `batch` | 256 | Maximum rows removed in each pruning step within a maintenance pass. |

See the [configuration parser](../src/config.cpp), [database design](../DATABASE.md),
[request processor](../src/processor.cpp), and [contact-log writer](../src/contact_log.cpp).
