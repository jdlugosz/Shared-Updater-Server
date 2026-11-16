# Shared Updater Server

See the [license](LICENCE) for source inspection and companion use.

Shared Updater Server is an Ubuntu daemon that reads a TOML catalog and returns
an Atom feed of available packages for an application suite. The
[Windows Client](https://github.com/jdlugosz/Shared-Updater-Client) selects and
verifies downloads. The primary HTTPS server forwards check-ins to the daemon's
loopback listener and serves the versioned download files separately.

## What the daemon does

- Validates the catalog at startup and preflights feeds against the response
  size limit. It returns offers for a suite, optionally filtered by platform.
  A supplied installation UUID may select a targeted track assignment.
- Handles overlapping connections and serializes check-in processing and
  persistence through a bounded queue. It returns a retry response when it
  cannot complete a check.
- Loads replacement catalogs in the background. It switches to a validated
  replacement between checks and keeps serving the current catalog if the
  replacement is rejected.
- Exposes a loopback health endpoint and reports readiness, reload completion,
  and watchdog progress to systemd.

## Census and contact log

An update check records an observation before the daemon returns success.
Anonymous checks use the all-zero UUID and increment a daily request count for
the suite in SQLite. Checks with a nonzero installation UUID record the suite,
UUID, first and last check times, and the UTC days on which it checked in. The
UUID can link repeat checks from one installation.

The daemon also keeps a size-limited contact log containing UTC time, suite,
and the supplied UUID, including the all-zero value for anonymous requests.
Neither record includes an IP address, account name, or installed version.
See [data and retention](docs/settings.md#census-and-contact-log) for the full
record and what the settings control.

## Reference

- [Daemon settings and recorded data](docs/settings.md)
- [Catalog inputs and publication](docs/catalog.md)
- [HTTPS and Atom protocol](docs/protocol.md)
- [Application integration](https://github.com/jdlugosz/Shared-Updater-Client/blob/main/INTEGRATION.md)

## Code for review

| Component | Responsibility |
| --- | --- |
| [catalog](include/updater/catalog.hpp) | Typed immutable model, fragment gathering, validation, diagnostics, and usage |
| [binary](include/updater/binary.hpp), [XML](include/updater/xml.hpp), [URI](include/updater/uri.hpp), [JSON](include/updater/json.hpp) | Reusable byte/text validation and formatting |
| [Atom](include/updater/atom.hpp) | Public/targeted selection, track overrides, stable IDs, bounded XML output |
| [processor](include/updater/processor.hpp), [persistence](include/updater/persistence.hpp) | One synchronous check, log synchronization, SQLite commit, and retention |
| [worker](include/updater/worker.hpp) | Bounded FIFO, admission lifetime, cancellation gate, activation, progress, shutdown |
| [HTTP](include/updater/http_server.hpp) | Asynchronous listener/sessions, framing, deadlines, completion delivery |
| [configuration](include/updater/config.hpp), [service](src/service.cpp) | Strict settings, lifecycle, loader deadlines, systemd, and watchdog |

XML checking lives in its own component. The TOML loader invokes it while source
locations are available, so invalid output text or extension names still receive
file/line diagnostics.

See the [daemon design](DESIGN.md), [request pipeline](REQUEST-PIPELINE.md),
[database design](DATABASE.md), and [test plan](TEST-PLAN.md).
