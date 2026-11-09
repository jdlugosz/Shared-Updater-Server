# Shared Updater Server

A working C++ daemon for Ubuntu behind Caddy. It assembles TOML catalog fragments,
returns Atom package feeds, records installation activity in SQLite, and maintains
a bounded contact log. One asynchronous I/O thread handles overlapping HTTP
connections; one worker serializes request processing and durable writes.

Catalog reloads validate on a separate loader thread. A rejected or timed-out
replacement leaves the active catalog serving requests. systemd supplies
readiness, reload acknowledgement, watchdog supervision, and restart.

## Build on VPS Local

Source is shared read-only from Styx at
`D:\Code Projects\Shared-Updater-Server` to
`/mnt/hgfs/Shared-Updater-Server`. Builds, test state, SQLite, and logs belong
on the guest's native filesystem.

```sh
cd /mnt/hgfs/Shared-Updater-Server
cmake --preset linux-gcc
cmake --build --preset linux-gcc
ctest --preset linux-gcc
```

The executable is
`/opt/shared-updater-server/build/gcc-16.2.0/linux-gcc/shared-updater-server`.

The existing PowerShell helper runs the normal build and tests through the
`vps-stage` SSH alias:

```powershell
pwsh -NoProfile -File .\tools\build-staging.ps1
```

Its `-Bootstrap` option installs the Ubuntu development prerequisites and
provisions the GCC 16.2 release build from Compiler Explorer through
`vps-stage-root`. The installer checks the archive's SHA-256 and places the
compiler and its runtime under `/opt/shared-updater-server/toolchains/gcc-16.2.0`.
Caddy is needed for the proxy acceptance tests.

The staging presets use GCC 16.2 in C++26 mode and keep its build trees separate
by compiler version. They set the runtime search path for built and installed
executables to the matching `lib64` directory. The optimized preset keeps
Ubuntu's fortification and linker hardening settings. A production install
needs a compatible C++ runtime on its target host.

Use the `linux-gcc-modules` preset for all three CMake and CTest commands
above to build with `import std;`. This opt-in preset uses the experimental
module support in staging's CMake 4.2 and has its own build directory.
In module builds, implementation files import `std` after project and
dependency headers. Includes that supply macros such as `errno` stay in
place. IntelliSense and the other presets read the ordinary headers.

Run `shared-updater-server --version` to read the version, build status and
configuration, UTC build date, full Git commit, checkout state, and exact tag
embedded in the executable. An exact tag supplies the version label. Untagged
builds use the CMake project version. A clean tagged `Release`, `RelWithDebInfo`,
or `MinSizeRel` build qualifies as a release when built without a sanitizer or
custom build variant. Label a profiling build with `-DUPDATER_BUILD_VARIANT=profile`,
or choose another descriptive label for a custom build. `SOURCE_DATE_EPOCH`
can supply a fixed timestamp for a reproducible build date.

## Local VS Code IntelliSense

Copy the [IntelliSense template](.vscode/c_cpp_properties.example.json) to
`.vscode/c_cpp_properties.json`. Set `env.stagingHeaders` to the local snapshot
root, with `gcc-16.2.0` from the release toolchain and `usr` from staging's
system headers. Git ignores this local copy so each developer can choose
their own path.

## Validate or emit a feed offline

These operations read configuration and catalog files without opening a listener
or creating census/log state:

```sh
/opt/shared-updater-server/build/gcc-16.2.0/linux-gcc/shared-updater-server \
  --config examples/server.toml --check-config

/opt/shared-updater-server/build/gcc-16.2.0/linux-gcc/shared-updater-server \
  --config examples/server.toml --feed Launcher
```

Add `--id UUID` to the feed command to inspect a targeted offering. The default
is Nil, the all-zero UUID. `--feed` prints the suite's Atom XML to stdout and
exits. A publication tool can validate a staged directory
with `--catalog-dir DIRECTORY --check-config`; that override is limited to
offline validation.

A successful load reports actual and configured counts, for example
`Success. 53/4096 fragments, 81/8192 kiBytes read.`
Partial kiBytes round upward. Usage and limits share a signed two-field type;
the immutable snapshot holds the actual policy used for its load.

The [example catalog](examples/catalog/10-launcher.toml) contains illustrative
download URLs, size, and checksum. Replace those fields before publication.

## Run as a service

The [service unit](deploy/shared-updater-server.service) uses
`Type=notify-reload`, a dynamic service user, private state/log directories,
and a watchdog. The [example configuration](examples/server.toml) documents
defaults. Relative paths resolve against the configuration file.

Install the selected build with the default `/usr/local` prefix:

```sh
sudo cmake --install /opt/shared-updater-server/build/gcc-16.2.0/linux-gcc
```

This installs the executable under `/usr/local/libexec`, the unit under
`/usr/local/lib/systemd/system`, operator tools under
`/usr/local/libexec/shared-updater`, and examples under
`/usr/local/share/shared-updater/examples`. It leaves site-specific
configuration for the operator.

Place the reviewed configuration at
`/etc/shared-updater-server/server.toml`. For slot publication, use
`catalog_dir = "/etc/shared-updater-server/catalogs/current"`.
systemd creates `/var/lib/shared-updater-server` and
`/var/log/shared-updater-server` privately. A manually launched process
requires writable private state/log directories owned by its user.

```sh
sudo systemctl daemon-reload
sudo systemctl start shared-updater-server
sudo systemctl reload shared-updater-server
journalctl -u shared-updater-server
```

The daemon writes the same build information as `--version` when it starts.

The [Caddy example](deploy/updater.caddy) forwards the full
`/updchk/<UUID>/<suite>` path to loopback, preserves concurrency, and filters
check-in identities from access and upstream-error logging. Merge it into the
existing site configuration with the actual hostname. Caddy also serves the
download files. Keep `/healthz` private.

## Tests and sanitizers

The normal suite contains 58 C++ cases and four integration groups, including
real HTTP, real HTTPS through Caddy, backups/reports, and Git publication.
All 62 pass on VPS Local in normal, ASan/UBSan, and ThreadSanitizer builds.
The [test plan](TEST-PLAN.md) records the verified behavior and environment.

```sh
cmake --preset linux-asan
cmake --build --preset linux-asan
ctest --preset linux-asan

cmake --preset linux-tsan
cmake --build --preset linux-tsan
ctest --preset linux-tsan
```

Each build uses a separate native directory. Sanitizer presets use one build
job. ThreadSanitizer is separate from AddressSanitizer.

The isolated systemd acceptance test additionally exercises readiness, healthy
idle watchdog operation, actual watchdog restart, reload acknowledgement,
shutdown, and backup restoration. It runs only as root on `vps-stage`,
creates a uniquely named temporary unit and state, and removes them afterwards:

```sh
sudo python3 tests/systemd_integration.py \
  --server /opt/shared-updater-server/build/gcc-16.2.0/linux-gcc/shared-updater-server \
  --unit-template deploy/shared-updater-server.service
```

Integration failures retain diagnostics under the build's `test-results`
directory; systemd failures retain a named directory under `/run`.

## Publish a committed catalog

[Publish catalog](tools/publish_catalog.py) reads top-level regular `.toml`
files from one selected Git commit, enforces catalog budgets, validates them
with the daemon, and records the source commit. Uncommitted changes are excluded.

The publication root holds `slot-a`, `slot-b`, and the `current` symlink.
The publisher fills the inactive slot and switches the symlink atomically under
a publisher lock. It requests a reload and verifies the active revision and
reload generation. Rejected activation restores the previous disk selection,
reloads it, and verifies the restored running revision.

```sh
sudo python3 tools/publish_catalog.py \
  --repo /srv/updater-catalog --ref RELEASE_COMMIT --catalog-path catalog \
  --config /etc/shared-updater-server/server.toml \
  --server /usr/local/libexec/shared-updater-server
```

For the first selection before starting the service, add `--initial`.
New or changed artifact URL/size/hash combinations are downloaded over HTTPS
and checked before publication. Existing verified combinations are retained.
`--skip-download-verification` is for fixtures or publication whose artifact
bytes were verified elsewhere.

Keep build IDs and artifact URLs immutable in Git. The daemon rejects changes
to existing active build versions, publication times, and artifact bindings,
and rejects reuse of its active revision for different catalog content.
Tracks and suite metadata can change with a new revision.

## View the live catalog

From the Windows development checkout, use PowerShell 7 and the existing
operator SSH login. The command captures the selected catalog slot, verifies
its revision against the running service, and opens a local HTML report.

```powershell
.\tools\show-catalog.ps1
```

The report shows the revision, publication and capture dates, source commit,
suites, packages, all build artifacts, and public and targeted track assignments.
Targeted installation IDs are hidden unless you pass `-IncludeTargetIds`.
Use `-NoOpen` to save the report without opening a browser. Rerun the command
to refresh the snapshot. The report goes to `out/catalog-report/live.html`.
The `-SshTarget` option selects another host with the same publication layout.

## Backups and census reports

[Census tools](tools/census.py) use SQLite's backup API and verify integrity and
foreign keys. Backups require a new destination in a private directory and
never overwrite an existing file. Reporting copies a consistent snapshot before
running queries, so report queries do not hold a long live-database read lock.

```sh
python3 tools/census.py backup /var/lib/shared-updater-server/census.sqlite /PRIVATE_BACKUP/census.sqlite
python3 tools/census.py check /PRIVATE_BACKUP/census.sqlite
python3 tools/census.py report /PRIVATE_BACKUP/census.sqlite --days 30
```

Reports separate retained installations, newly observed installations, distinct
active installations, monthly activity, and anonymous request totals by suite.
Windows are half-open UTC day intervals; the default includes today.
`--cohort START END RETURN_START RETURN_END` adds returns for a completed,
disjoint return window. Supply the actual `--activity-days` and
`--inactive-days` policies so the tool can reject windows outside retention.

Restore a verified backup while the service is stopped, preserving the service
user's file ownership and private permissions. Keep the prior database until
the restored service has passed its health and integrity checks.

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
