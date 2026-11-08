# Shared Updater Server test plan

The target is a repeatable pass from C++ unit tests through a real Ubuntu service and Caddy. See the [daemon requirements](DESIGN.md), [request pipeline and test interfaces](REQUEST-PIPELINE.md), and [SQLite design](DATABASE.md). Use synthetic UUIDs, suites, artifacts, and dedicated state directories in automated tests.

## GCC 16.2 verification on November 6

All 63 CTest cases passed on VPS Local with the GCC 16.2 release in each of
the normal, AddressSanitizer/UndefinedBehaviorSanitizer, and ThreadSanitizer
presets. A separate tagged checkout verified that `--version` reflects tag
and checkout changes on rebuild and that identical metadata does not relink.
The temporary installation used the matching C++ runtime and passed
the isolated systemd acceptance test. The normal binary is
`/opt/shared-updater-server/build/gcc-16.2.0/linux-gcc/shared-updater-server`.

## Implemented server verification on November 5

| Check | Result |
| --- | --- |
| Normal GCC 16/C++26 preset | All 62 CTest cases passed: 58 C++ cases and four integration groups |
| AddressSanitizer and UndefinedBehaviorSanitizer preset | The same 62 cases passed with leak detection and halt on undefined behavior |
| Separate ThreadSanitizer preset | The same 62 cases passed with halt on the first race report |
| Real loopback HTTP | Concurrent checks, request validation, Nil, FIFO saturation, queue expiry, connection reset during persistence, restart, private health, and valid/invalid reloads passed |
| Real HTTPS Caddy and static download | Parsed the feed, fetched its enclosure, checked size/SHA-256, verified concurrent forwarding, and checked UUID absence in operational logs including an upstream 502 |
| Git publisher | Exported selected committed fragments; validated and activated a second slot; rejected symlinked or invalid fragments; restored both disk selection and running revision after an immutable-artifact rewrite was rejected |
| Census operator tools | Verified new private backups, refusal to overwrite an existing backup, active/new/anonymous/monthly/cohort reports, snapshot checks, and restart after an offline restore |
| Isolated systemd unit using the packaged hardening | Readiness, idle survival across watchdog periods, successful and rejected reload acknowledgement, actual watchdog termination/restart during a blocked writer, graceful stop, and backup restoration passed |
| Temporary CMake installation | Executable, tools, unit permissions, and examples installed correctly; the installed executable validated its installed example catalog |

That run used
`/opt/shared-updater-server/build/linux-gcc/shared-updater-server`.
The acceptance test creates a uniquely named unit on VPS Local and removes its
unit, state, and logs. It leaves the existing Caddy service unchanged.

Expanded testing caught the systemd DynamicUser directory aliases and a
ThreadSanitizer report when consuming a loader result before joining its exited
thread. The implementation now resolves and validates the service-owned
directories and joins the completed loader before reading its result. The final
normal and sanitizer suites and systemd acceptance test passed after these fixes.

The complete CTest suite is available through `linux-gcc`, `linux-asan`,
and `linux-tsan` configure/build/test presets. The sanitizer builds are separate
and use one compilation job. GCC emits its existing unsupported-fence
instrumentation warning from Boost.Asio headers in the ThreadSanitizer build;
the tests run without race suppressions.

## VPS Local baseline and design probes

On November 4, 2026, VPS Local (`vps-stage`) reported Ubuntu 26.04.1 LTS, two CPUs, GCC 16.0.1 from the pinned Ubuntu snapshot, CMake 4.2.3, systemd 259, Caddy 2.11.7, SQLite 3.46.1, toml++ 3.4.0, and Catch2 3.7.1.

| Probe | Observed result |
| --- | --- |
| Existing built Catch2 smoke test | Passed from the guest's ext4 build directory |
| Temporary Caddy instance and HTTP backend | Forwarded two simultaneous requests with unchanged paths. A rendezvous at the backend required both requests to arrive concurrently. |
| Temporary Caddy upstream failure | Access-log skipping alone left the UUID in the 502 error. The scoped runtime filter removed the request object while retaining the status and error diagnostic. |
| SQL copied from the design into a scratch SQLite database | Passed migration, constraints, repeated same-day contacts, timestamp extrema, transaction rollback, active and cohort reports, Nil counting, backup, bounded pruning, and cascading deletion |
| Temporary GCC 16 build using `-std=c++26` | Compiled and ran a probe using `std::jthread`, stop tokens, `std::expected`, and `std::println` |
| Illustrative configuration | The example catalog parsed and its track referenced the sample artifact. Caddy validated the proposed proxy route in a temporary site. |

VMware listed the source export, but Ubuntu had no HGFS mount at `/mnt/hgfs` during inspection. Restore the mount or stage a reviewed source snapshot before rebuilding. Boost, libxml2, and libsystemd development packages were available in APT and require a build/link check during implementation. Production toolchain compatibility needs its own preflight.

The catalog implementation was built after the source share was mounted again.
All 21 discovered CTest cases passed on VPS Local with GCC 16 in C++26 mode,
including 20 catalog cases and the existing dependency smoke test. The same
suite passed in a separate AddressSanitizer and UndefinedBehaviorSanitizer
build. Catalog tests exercise the public file and directory loaders.

The success-report addition brings the normal suite to 25 passing cases. It
covers actual fragment and byte counts, the requested 53-fragment example,
custom limits, partial-kiByte rounding, and maximum-size arithmetic.

The November 5 `catalog.hpp` review changes bring the suite to 27 passing cases
in both the normal GCC 16/C++26 build and a separate AddressSanitizer and
UndefinedBehaviorSanitizer build. The tests check key/value associations across
all snapshot maps after gathering unordered records, signed usage reporting,
negative limits, zero ceilings, and the maximum signed byte count. Snapshots
now use `std::flat_map`, and usage and limits share the same signed count type.

## Automated unit and component tests

Extend the existing Catch2 target and CTest discovery. Inject wall and monotonic clocks. Use scratch directories for filesystem tests and barriers or events for concurrency assertions. Keep fault controls in the test harness or test executables.

| Area | Cases and expected results |
| --- | --- |
| Route parser, R4–R5 | Accept hyphenated hexadecimal IDs regardless of UUID version or variant. Nil represents anonymous check-ins. Normalize uppercase suite text and accept repeatable canonical platform IDs. Reject malformed IDs, invalid suite, extra segments, malformed or unsupported queries, percent encoding, disallowed method/body, and excess size. Invalid input never reaches the recorder. |
| Catalog assembly, R2/R8 | Merge split files and resolve cross-file references. Detect duplicate records with both source locations, missing artifacts, dangling references, invalid timestamps/digests/links, invalid XML text, unsupported versions, and resource limits. Output is independent of input order. |
| Reload and publication, R1–R2 | Export a selected commit from a fixture Git repository and verify its source-commit record. Gate candidate validation and prove that check-ins complete on the old revision while the gate is closed. Activate a complete replacement. Reject invalid candidates, timed-out validation, and failed database activation while continuing to serve the old revision. Verify restoration of the current slot on rejection and a restart from the restored slot. Verify that queued work chooses the active snapshot at worker start. |
| Feed, R4/R8 | Parse XML and verify namespaces, links, length, checksums, revision, UTC dates, stable IDs, escaped Unicode and punctuation, and empty feeds. Check track reassignment and older build visibility. Nil and unrelated IDs receive no targeted-only entries. Required capability metadata survives serialization. |
| Queue and worker, R3 | Release callers together and verify atomic admission, assigned-sequence FIFO work, peak active work of one, and overflow. Hold completed results in a test mailbox and prove they still consume capacity despite an empty deque. Cover exact queue deadlines, cancellation at the persistence gate, physical removal of cancelled jobs, completion ownership, slot recovery, and pending writes. A gated slow writer leaves the worker free for other jobs. |
| Rolling log, R6 | Test exact byte boundary, previous/current contents, one record per processed contact, restart, and incomplete-tail repair. Inject short writes, interrupted writes, rename failure, and sync failure. |
| Database, R5–R7 | Apply actual migration. Test constraints, same-day deduplication, suite isolation, Nil increments, timestamp extrema, restart persistence, and refusal of unknown schema versions. Force failure between the two real-ID statements and exercise an on-disk writer lock. |
| Statistics and pruning, R7 | Use fixed UTC boundaries and known cohort outcomes. Verify anonymous separation, cutoff equality, bounded batches, cascade, re-observation after expiry, and restored backup integrity. |
| Persistence sequencing, R6–R7 | Log failure leaves census unchanged and returns busy. SQL failure may leave a logged attempt and returns busy. Success follows commit. A lost response followed by retry preserves the distinct measures. |
| Watchdog and stop, R1 | Fake clock covers idle, active progress, an active stall with empty queue, waiting work with no worker progress, and an overloaded worker that still advances. Verify bounded shutdown and reload progress. |

Run unit and component tests in the normal build workflow. The sanitizer presets cover session lifetime, queue ownership, and shutdown. Daemon/proxy tests have separate labels so Caddy is a prerequisite only for the proxy layer.

Feed tests call the real synchronous writer directly and parse its XML. Processor and worker tests use typed requests, controlled persistence adapters, barriers, and an injected completion mailbox. They require no sockets. Add the real HTTP component early and drive it on an assigned loopback port through the same worker. This socket layer checks request framing, body refusal, queue-to-session delivery, half-close, timeouts, write-buffer lifetime, and shutdown. Force a held write with a test gate or controlled stream; an unread small response can still fit in the kernel buffer.

The separate ThreadSanitizer configuration covers shared queue, cancellation, control state, and the real HTTP/reload path. Keep it separate from AddressSanitizer. The [pipeline verification matrix](REQUEST-PIPELINE.md#automated-verification) specifies the required races and capacity assertions.

## Automated daemon and Caddy harness

Use a Python standard-library harness under CTest on Linux. Create an isolated catalog, test download, local state, and temporary loopback ports. Start the real daemon in foreground mode. Wait for readiness with a deadline and check its revision. Start a separate Caddy instance with temporary storage/config directories, automatic HTTPS disabled, and its administration endpoint disabled for the loopback HTTP test. Clean up child processes on success or failure and retain diagnostics on failure.

1. Exercise the daemon directly and through Caddy. Validate the Atom response and its cache policy, then fetch a static fixture through Caddy and compare its size and SHA-256 with the feed.
2. Check the same UUID twice for Launcher, once for another suite, and twice with Nil. Inspect a consistent database snapshot and the contact log. Expect two distinct real pairs, two distinct real day rows, and two anonymous requests.
3. Send invalid requests and confirm the expected status and absence of census effects. Verify that Caddy forwards the update path and platform query and that the private health endpoint stays off the public route.
4. Send simultaneous valid requests through Caddy and verify coherent records. Use a gate to fill the admitted queue deterministically. Assert `503` and `Retry-After` for overflow and a successful check after capacity returns.
5. Publish a replacement catalog from Git with a changed track and an archived build. Send check-ins through Caddy during validation and activation. Verify that each response contains one complete revision, the daemon PID stays the same, and health remains ready. Repeat with an invalid replacement and require successful check-ins against the old revision throughout. Switch back to the previous slot and verify the selected revision after a restart.
6. Rotate with a tiny contact-log cap, then restart. Verify both file sizes, database first/last contact, activity rows, and log continuation.
7. Inspect the isolated Caddy logs and daemon diagnostics for exposure of raw UUIDs or client IPs on the check route, including an upstream error. The controlled contact log should contain the expected UUID and suite.

Use fixture clocks or UTC-safe fixture windows so tests remain deterministic across midnight. Exercise termination after log append and before commit, then after commit and before the HTTP response. Validate the expected attempted contact or committed observation on restart. Use bounded waits and preserve logs on failure.

## VPS Local final acceptance

After implementation, verify the source mount and run the build and test commands:

```sh
findmnt /mnt/hgfs
cd /mnt/hgfs/Shared-Updater-Server
cmake --preset linux-gcc
cmake --build --preset linux-gcc
ctest --preset linux-gcc
```

Use a separate staging systemd unit with its own state and a test catalog. Validate the unit and Caddy configuration. The proposed isolated test ports below are 18080 for the daemon and 18081 for Caddy.

```sh
curl -fsS http://127.0.0.1:18080/healthz
curl -fsS \
  http://127.0.0.1:18081/updchk/12345678-1234-4234-8234-1234567890ab/Launcher \
  -o /tmp/updater-stage.atom
python3 -c 'import xml.etree.ElementTree as E; print(E.parse("/tmp/updater-stage.atom").getroot().tag)'
```

Expect `{http://www.w3.org/2005/Atom}feed` and verify the revision and links against the fixture. Run the automated proxy matrix. Then verify the service lifecycle on the actual systemd manager:

- Readiness follows database, catalog, and listener initialization. Invalid startup configuration fails with bounded restart attempts.
- Valid and invalid reloads through `systemctl reload` produce the expected active revision and reload result. Inspect both rather than relying solely on the command exit status.
- A deliberately stalled test worker triggers watchdog termination and restart in an isolated unit. A healthy idle worker survives longer than the watchdog. Verify a successful new check and database integrity after restart.
- Stop and restart under a small workload. Verify the shutdown budget, completed transactions, and retry of interrupted responses.
- Restore a staging backup into isolated state. Verify integrity, foreign keys, and representative reports.

Record OS, compiler, package and service versions, catalog revision, automated results, and the expected synthetic database and log observations.

## Production promotion after review

Build the reviewed source for the production OS. Verify runtime dependencies, offline catalog validation, and a restorable database backup. Retain the prior binary and catalog. Install through systemd, verify the active revision on loopback, and activate the validated Caddy route. Check the public HTTPS endpoint with a designated synthetic contact. Parse the response, fetch a known installer URL, and confirm the database observation and log bounds. Keep load, watchdog, and deliberate failure tests on staging.

For a failed promotion, restore the prior route, binary, and catalog as applicable. A schema change requires a compatible older binary or restoring the pre-deployment database backup.
