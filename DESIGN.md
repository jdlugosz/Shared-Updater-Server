# Shared Updater Server design

Implementation and review reference updated November 5, 2026. The C++ daemon is implemented and tested on VPS Local; production promotion follows review. The desktop updater invokes the service and handles package selection, downloads, installation, and rollback. Caddy serves the website and installer files and forwards update checks to the daemon.

The [database design](DATABASE.md) specifies the SQLite schema, transactions, retention, and reports. The [request pipeline design](REQUEST-PIPELINE.md), detailed on November 5, specifies thread ownership, FIFO admission, completion delivery, cancellation, and reusable test interfaces. The [test plan](TEST-PLAN.md) specifies automated checks and the VPS Local acceptance procedure.

## Requirements

| ID | Requirement |
| --- | --- |
| R1 | Run as a foreground process supervised by systemd. Report readiness after catalog validation, database initialization, and listener startup. Support reload, orderly stop, restart on failure, and a progress watchdog. |
| R2 | Read any number of catalog fragments within configured resource limits. Assemble one coherent catalog from their records. Continue serving the active revision while validating a replacement and after a rejected reload. |
| R3 | Accept concurrent Caddy connections and serialize admitted check-in work through one FIFO worker. Bound connections, unfinished jobs, request size, and wait time. Return a busy response when admission is full. |
| R4 | Accept an installation UUID, Application Suite ID, and optional platform IDs. Return an Atom package feed with direct download and information links, build identifiers, platforms, tracks, and extension metadata. |
| R5 | Support the all-zero Nil UUID. Return the public feed and count anonymous requests separately from installation activity. |
| R6 | Append each valid check-in that reaches persistence to a size-limited contact log. Keep a current file and one previous file. Update durable census summaries before returning success. |
| R7 | Preserve first and last contact and enough dated activity to measure active installations and returns. Make retries safe for distinct-installation measures. |
| R8 | Publish immutable build identities and retain applicable older artifacts for rollback. Allow non-confidential installation-targeted track selections. |
| R9 | Build and test on VPS Local before promoting the same reviewed source and catalog snapshot to a compatible production environment. Exercise the daemon directly and through Caddy. |

## HTTP contract

| Request or outcome | Response and effects |
| --- | --- |
| `GET /updchk/<GUID>/<suite>` for a known suite | `200`, `application/atom+xml; charset=utf-8`, and `Cache-Control: no-store`. Record a contact even if the catalog content has not changed. An empty suite has a valid empty feed. |
| Nil UUID on the same route | Same public feed and logging path. Increment a separate anonymous daily request count. |
| Invalid UUID, invalid suite token, invalid or unsupported query parameter, percent-encoded target, or request body | `400`. A parser-level size violation can return `431` or close the connection if a valid response cannot be sent. |
| Valid syntax with unknown suite or route | `404`. |
| Unsupported method on the check route | `405` with `Allow: GET`. A HEAD response has no body. |
| Admission full, expired queued job, or transient persistence failure | `503` with `Retry-After: 30` as the initial default. No feed is returned. |
| Loopback `GET /healthz` | `200` when initialized and accepting work, otherwise `503`. Include active catalog revision and a coarse reload result for deployment checks. No census side effects or identifiers. |

Accept hyphenated UUIDv4 text, with hexadecimal case ignored, plus the explicitly allowed Nil UUID. Normalize to 16 bytes and use lowercase text in the contact log. Suite IDs are ASCII tokens of 1–64 characters, beginning with a letter and continuing with letters, digits, underscores, or hyphens. ASCII case is ignored and the canonical catalog ID is lowercase. Thus `/updchk/00000000-0000-0000-0000-000000000000/Launcher` selects `launcher`. Require exactly the two path segments after the prefix.

The optional query consists of one or more `platform=ID` parameters joined by `&`. IDs use the catalog's canonical lowercase ASCII spelling. They are 1–64 characters, begin with a letter or digit, and can contain letters, digits, underscores, and hyphens. The target limit applies to the path and query together, with a default of 256 bytes.

```http
GET /updchk/<UUID>/Launcher?platform=windows-x64-avx2&platform=windows-x64
```

An unfiltered request receives the full suite feed. A filtered request includes artifacts with a matching platform ID and all `any` artifacts. A valid unknown ID can match just the `any` artifacts, and a known suite with no eligible artifacts returns a valid empty feed. Filtered checks count by UUID and suite, including checks with an empty result. Clients select the build and track, prefer compatible variants in their own order, and check any required capabilities before installing.

The daemon listens on loopback only, with `127.0.0.1:8080` as a proposed default. Caddy preserves the entire URI and terminates production HTTPS. Forward only `/updchk/*`, leave `/healthz` private, and serve installer files through Caddy's existing file routes. Disable upstream retries on the check route. Skip its access logs and filter the site's runtime error logger so the bounded daemon contact log is the deliberate identity record. Daemon operational errors use categories and request sequence numbers. [Caddy reverse proxy](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy) · [Caddy log_skip](https://caddyserver.com/docs/caddyfile/directives/log_skip)

Use a named site logger for the updater site. Merge these loggers into the existing global options. The observed upstream failure on VPS Local included the UUID in the runtime error with only `log_skip`. The scoped runtime filter below retained the 502 diagnostic while removing the request object, including URI and client address. Exercise the effective production logging setup in staging, including error paths and any additional log outputs. [Caddy log filters](https://caddyserver.com/docs/caddyfile/directives/log#filter)

```caddyfile
{
    log default {
        exclude http.log.error.updater
    }
    log updater_runtime {
        include http.log.error.updater
        output stderr
        format filter {
            wrap json
            fields {
                request delete
            }
        }
    }
}
```

Site block fragment, with the hostname supplied at deployment:

```caddyfile
log updater
log_skip /updchk/*
handle /updchk/* {
    reverse_proxy 127.0.0.1:8080 {
        lb_retries 0
        transport http {
            dial_timeout 2s
            response_header_timeout 25s
        }
    }
}
```

## Catalog inputs and Atom output

The installed scaffold already uses toml++. Use TOML for the complete editable catalog and Atom for the delivered feed. Both represent the same package model. The daemon accepts an explicit list of fragments or a directory of `.toml` files. It sorts directory inputs by filename for reproducible diagnostics. It parses each file, combines typed records, then resolves cross-file references. Duplicate keys are errors, with both source locations reported. File order does not confer override priority.

| Record | Key and fields |
| --- | --- |
| `[catalog]` | One per assembled catalog. Required `schema_version = 1`, `revision`, `published_at` as an offset-aware TOML timestamp, and publisher `author`. |
| `[[suite]]` | Canonical `id`, `title`, and list of package IDs. A package can appear in several suites. |
| `[[package]]` | Stable `id`, `title`, and `kind` such as installer, helper, source, documentation, or examples. |
| `[[build]]` | `package` and immutable `id` form the key. Include display `version`, `published_at`, `about_url`, and optional `visibility` of public or targeted, defaulting to public. Optional `requires` lists client capabilities. |
| `[[build.artifact]]` | One per platform within its build. Include `platform` such as windows-x64, windows-arm64, or any, HTTPS `url`, `media_type`, byte `size`, and hexadecimal `sha256`. |
| `[[build.extension]]` | Optional simple-text Atom metadata. Include an absolute HTTPS `namespace`, an XML local `name` without a prefix, and `text`. An optional `suite` restricts the element to one suite containing the package. |
| `[[track]]` | `suite`, `package`, `name`, and `platform` select a `build`. Optional non-Nil `installation_id` qualifies an override for that UUID. |

```toml
# 00-catalog.toml
[catalog]
schema_version = 1
revision = "sample-001"
published_at = 2026-11-04T18:00:00Z
author = "Długosz"

[[suite]]
id = "launcher"
title = "Launcher"
packages = ["launcher-app"]

# A separate fragment can contain the following package records.
[[package]]
id = "launcher-app"
title = "Launcher"
kind = "installer"

[[build]]
package = "launcher-app"
id = "sample-build-001"
version = "1.2.3"
published_at = 2026-11-04T18:00:00Z
about_url = "https://example.invalid/launcher/sample-build-001"

[[build.artifact]]
platform = "windows-x64"
url = "https://example.invalid/downloads/launcher-sample-build-001.exe"
media_type = "application/octet-stream"
size = 1234
sha256 = "0000000000000000000000000000000000000000000000000000000000000000"

[[track]]
suite = "launcher"
package = "launcher-app"
name = "current"
platform = "windows-x64"
build = "sample-build-001"
```

The example uses fixture URLs and digest. Check unique suite, package, build, artifact, and track keys. Validate sizes, HTTPS links, timestamps, SHA-256 format, XML-representable text, suite membership, and every reference. A track must point to an existing artifact for its platform. A default track cannot expose a targeted-only build. The same revision must always refer to the same published content. Publisher tooling verifies immutable artifact URLs, sizes, and hashes before publication. The daemon validates the metadata locally without reaching out to the artifact host on reload.

Include the suite's public artifacts, including retained older builds, in its feed. Add targeted artifacts only when their track assignment matches the caller. For a matching UUID, a targeted assignment replaces the track tag for that suite, package, and platform. The public artifact may still appear without that tag. Resolve an exact platform before considering a platform-independent assignment on the client. Track changes never rewrite a build ID or remove its archived artifact by themselves. Targeting with a UUID is intended for non-confidential builds. Restricted downloads require a separate authorization design.

Emit one Atom entry per eligible artifact. Use stable feed and entry IRIs under a configured publisher-controlled HTTPS base, independent of UUID and track. Feed title, author, and updated time come from the catalog. Each entry has an ID, title, updated time, alternate link for the information page, and enclosure link with download URL, media type, and byte length. Use libxml2's XML writer for escaping and namespace handling. [Atom RFC](https://www.rfc-editor.org/rfc/rfc4287.html) · [libxml2 XML writer](https://gnome.pages.gitlab.gnome.org/libxml2/html/xmlwriter_8h.html)

The proposed core namespace is `https://dlugosz.enterprises/ns/updater/1`, with `upd` as the illustrative prefix. Feed-level fields are `suite` and `revision`. Entry fields are `package`, `kind`, `build`, `version`, `platform`, and `sha256`. The `track` and `requires` elements can repeat. Keep these field meanings stable. Use the catalog publication time for feed and entry `updated`, and the build publication time for entry `published`. Keep source times consistent by rejecting a catalog timestamp earlier than any included build timestamp.

Suite extensions use separate namespaces. Reserve the core and Atom namespaces, and reject duplicate extension names in the same namespace within a suite view of an entry. Keep unknown optional metadata ignorable. Clients must understand every required capability before automatically installing an artifact. The TOML loader rejects unknown core field names and supports extension text through the explicit extension record. Structured extension content can be added in a future version.

The implemented catalog loader defaults to 8 MiB across up to 4096 fragments. Usage and limits share one type with signed byte and fragment counts; the default limits are a compile-time constant. Identifiers use up to 64 lowercase ASCII letters, digits, underscores, or hyphens, beginning with a letter or digit. Suite IDs begin with a letter. Validate field sizes and XML 1.0 text. Links use absolute HTTPS URLs with ASCII host and URL syntax, including percent escapes where needed. Artifact media types use type/subtype. The loader gathers definitions in mutable maps, then moves their sorted keys and values into `std::flat_map` vectors for the immutable snapshot. The model owns its values.

The Atom writer preflights each suite against the 512 KiB response ceiling by rendering a conservative view containing every artifact and applicable tag in that suite. Its runtime output sink also enforces the cap while serializing. XML text/name validation is factored into a reusable component; the TOML loader invokes it while source locations are available. UUIDs and digests use arrays of `std::byte`.

## Git history and catalog publication

Keep the TOML fragments in Git and treat a selected commit as one complete catalog revision. The implemented publisher reads top-level regular `.toml` objects from the selected tree with `git ls-tree` and `git cat-file`, enforcing the catalog byte/count budget before export. It runs `--catalog-dir STAGING --check-config` against that snapshot and records its exact source commit. The commit is deployment provenance and the catalog's own revision label remains part of the feed. New or changed artifact URL/size/hash combinations are downloaded over HTTPS and verified, unless the operator explicitly selects fixture/offline publication.

Use two reusable deployment slots under `/etc/shared-updater-server/catalogs/`. The `current` symlink selects the active slot. Under a single publisher lock, fill the inactive slot with the verified export, switch `current` atomically, then request reload and verify the active revision. The formerly active slot is available for quick rollback. To restore an older revision, publish its Git commit through the same procedure.

The daemon resolves `current` once per load and reads ordinary TOML files from the selected slot. The slot remains fixed while it is active. Startup reconstructs the snapshot from the selected files. A failed reload keeps the active in-memory snapshot and reports rejection. The publisher then restores the old `current` target and verifies the running revision so the next startup uses the last accepted catalog. Git operations belong to publishing.

### Live reload without service interruption

Keep the HTTP listener and request worker serving the active catalog throughout a reload. A separate, short-lived loader thread reads, parses, and validates the candidate while requests continue. Allow one candidate build at a time and coalesce repeated reload requests. If validation fails or exceeds its deadline, report rejection and discard the candidate, including any late result.

Hand a complete candidate to the request worker for activation between jobs. Register its suite IDs in a bounded SQLite transaction, then swap the active snapshot after commit. If activation fails, keep serving the old revision. In-flight responses finish with the bytes they already prepared. Later jobs use whichever catalog is active when they start.

Keep the private health endpoint ready during reload and expose the active revision and reload outcome separately. An unsuccessful reload is an operator-visible configuration event. The watchdog still measures progress of the HTTP loop and request worker while the candidate is being built.

The command line is `shared-updater-server --config FILE` for foreground service and `shared-updater-server --config FILE --check-config` for offline validation. Offline validation checks configuration, catalog assembly, and feed preflight without binding sockets or writing census state. `--feed SUITE [--id UUID]` emits a feed offline. Require exactly one catalog source setting, either an ordered file list or a directory. Reload replaces the catalog only. Listener, state paths, and resource-policy changes are applied on restart.

Illustrative daemon configuration. Resolve relative paths against the directory of the configuration file. An explicit `catalog_files` list can replace `catalog_dir`.

```toml
listen = "127.0.0.1:8080"
catalog_dir = "/etc/shared-updater-server/catalogs/current"
publisher_id_base = "https://dlugosz.enterprises/updater/id/"
state_dir = "/var/lib/shared-updater-server"
log_dir = "/var/log/shared-updater-server"

[limits]
connections = 128
unfinished_requests = 64
contact_log_bytes_per_file = 1048576

[retention]
activity_days = 400
inactive_installation_days = 730
```

## Request execution and persistence

Use one Boost.Asio I/O thread and one processing worker. Boost.Beast parses HTTP, limits headers and bodies, and asynchronously writes responses. The I/O side validates request shape, assigns an internal sequence, captures the contact timestamp at admission, and submits an owning message to a bounded FIFO implemented with a deque, mutex, and condition variable. The worker posts owning replies back to the I/O executor and never waits for socket writes. Check capacity and insert under the same lock. One shared admission lease remains reserved while a queued job, active operation, posted completion, or pending write still owns its resources; disconnection alone cannot release an active worker's reservation. Track live connections independently, including clients that have yet to send valid headers. See the [detailed handoff and lifetime rules](REQUEST-PIPELINE.md). [Boost.Beast documentation](https://www.boost.org/latest/libs/beast/doc/html/index.html)

| Proposed starting limit | Value |
| --- | --- |
| Open backend connections | 128, with one request per connection in v1 |
| Admitted unfinished check-ins | 64, including the active worker job and pending writes |
| Request target and headers | 256 bytes for the target and 8 KiB for the header block |
| Header read / queue wait / response write | 5 s / 5 s / 10 s |
| Expected worker operation budget | 10 s before watchdog health is lost |
| SQLite busy timeout | 2 s |
| Bounded contact log | 1 MiB per file, at most 2 MiB total |

Process requests in admission order on the worker. Select the active catalog when the worker starts the job, so an already queued request can use a newer successfully activated revision. Remove cancelled and expired queued messages physically from the FIFO. A cancellation flag checked under the queue mutex gates the transition from preparation into persistence; cancellation that wins that race has no census effects. Once the worker starts persistence, let the sequence finish even if the connection disappears. A remote disconnect may only become observable later, and a half-close after a complete request does not imply cancellation. Network writes take place on the I/O side. Slow clients can hold their own admission slots up to the write deadline without blocking the worker. Wire response order across connections can differ from persistence order.

For a known suite, render the response before writing data. Append one newline-terminated JSON contact record, then commit the census transaction, then release the successful response to the I/O thread. A representative line is `{"at":"2026-11-04T18:00:00Z","id":"00000000-0000-0000-0000-000000000000","suite":"launcher"}`. Reject invalid requests before contact logging. Do not record request bodies, client IPs, or referring URLs.

The worker owns both log files. Before a complete line would cross the per-file cap, close and rename the current file over the previous one in the same directory, then open a new current file. Use private file permissions, handle interrupted and short writes, and synchronize the appended line before attempting SQL. Synchronize the directory when rotation creates or replaces names. Recover a trailing incomplete line on startup, and stop accepting successful check-ins if the writer cannot recover. Log failures yield `503` before touching census state. A database failure can leave a logged attempt and yields `503`. The log is bounded contact history and the database is the source of committed statistics. This ordering deliberately avoids trying to make the filesystem log and SQLite one transaction.

## Service lifecycle and deployment

Use systemd with `Type=notify-reload`, `NotifyAccess=main`, `WatchdogSec=30s`, and `Restart=on-failure`. Use a finite restart delay and start-rate limit. Send `READY=1` after initialization. Handle SIGHUP in the event loop and start candidate validation on the separate loader. Send the reload notifications required by systemd, including the monotonic timestamp when starting reload and `READY=1` when the attempt completes. A completed reload attempt can report success to systemd even when the application retained the old catalog, so the publishing procedure verifies the active revision and reload result through the private health endpoint and journal.

Schedule watchdog heartbeats on the I/O loop and consult worker progress tracked by a monotonic clock. Idle with an empty queue is healthy. Pending work with no worker progress or an active job exceeding its deadline withholds the heartbeat, including when the queue is empty behind that active job. Completed failures count as progress and are reported separately from a stuck worker. Apply a bounded startup and shutdown timeout. On SIGTERM, stop admission, reject or cancel unstarted work, let the current transaction complete within the shutdown budget, and close the writer and connections. The service manager handles the hard deadline. [systemd service manual](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)

Proposed service footprint:

| Path or setting | Purpose |
| --- | --- |
| `/usr/local/libexec/shared-updater-server` | Versioned release binary installed through the deployment procedure |
| `/etc/shared-updater-server/server.toml` | Listener, catalog location, publisher base IRI, resource limits, and retention |
| `/etc/shared-updater-server/catalogs/` | Two reusable read-only catalog slots, source-commit records, and the current slot reference |
| `StateDirectory=shared-updater-server` | Local SQLite state under `/var/lib/shared-updater-server` |
| `LogsDirectory=shared-updater-server` | Current and previous contacts logs |
| Dedicated service account and `UMask=0077` | Access restricted to the daemon and operators |

Keep source sharing on the host and build output, databases, and logs on the VM's native filesystem. An exclusive lock in the state directory prevents two daemon instances from owning the same files. The service user can read configuration and write only its state and logs. Use systemd filesystem restrictions consistent with those paths. Production promotion includes verified configuration, backed-up SQLite state, Caddy validation and reload, and a read-only health/revision check before a synthetic test contact. Keep the previous binary and catalog available. A schema change requires a compatible downgrade or a deliberate database restore.

## Code organization and toolchain

| Component | Planned ownership |
| --- | --- |
| `main` and service controller | CLI, config, startup order, signals, readiness, watchdog, stop and reload orchestration |
| `http` and `request` | Beast sessions, deadlines and bounds, pure UUID and route normalization, owned request/reply values, completion adapter, result-to-HTTP mapping |
| `catalog` and `atom` | TOML assembly, background candidate validation, audience/track resolution, immutable snapshots, XML output |
| `worker` and `processor` | Bounded FIFO, admission leases, synchronous check processing, persistence gate, progress, control handoff, return results to I/O loop |
| `census` | SQLite RAII connection/statements/transaction, migrations, recording, retention, report queries |
| `contact_log` | Bounded append, rotation, startup recovery, and I/O error propagation |

Keep policy in a core CMake library linked by the daemon and existing Catch2 target. The executable supplies sockets, process signals, and the systemd adapter. Favor standard library value types, RAII for resources, `std::expected` for recoverable boundary errors, `std::jthread` and stop tokens where ownership warrants them, and `std::chrono` for clocks. Keep the current C++26 preset on GCC 16. Use established C++23 facilities for core code and use C++26 features when a compile probe confirms availability on both build targets.

The implementation links SQLite, toml++, Boost.Asio/Beast, libxml2, and libsystemd, with Catch2, CMake, Ninja, Python, Git, and OpenSSL for testing and operator tools. VPS Local verified Boost 1.90, libxml2 2.15.2, and systemd development package 259.9 with the pinned GCC 16 snapshot. The processor/FIFO is shared by direct component tests, the real loopback listener, and the daemon; real persistence and controlled adapters use the same contracts. The [pipeline implementation sequence](REQUEST-PIPELINE.md#implementation-sequence) records these boundaries. The [test plan](TEST-PLAN.md) records the complete staging checks.

## Decisions for review

TOML source with Atom delivery follows the existing scaffold. The 400-day activity history, 730-day inactivity cutoff, byte limits, port, and timeouts are configurable defaults. Production still requires a compatible toolchain, actual package assets, reviewed site configuration, and verified backups. The design assumes a single server and one writer. Confidential builds require a separate authorization design.
