# Shared Updater Server test plan

This reference covers unit, component, and integration checks. The tests use
synthetic identifiers, suites, and artifacts with dedicated state directories.
See the [daemon design](DESIGN.md), [request pipeline](REQUEST-PIPELINE.md), and
[SQLite schema](DATABASE.md).

A November 14 staging run passed all 69 CTest cases, including HTTP and Caddy
integration, Git publication, catalog reporting, census tools, and build metadata.

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
