# Catalog inputs and publication

The daemon assembles a read-only catalog from TOML fragments. It validates the
assembled relationships and then uses the catalog to generate Atom feeds.
The [example catalog](../examples/catalog) shows how to split records across
files. The [daemon settings](settings.md) describe source selection and resource
limits.

## Catalog records

| Table | Fields |
| --- | --- |
| `[catalog]` | Exactly one. Set `schema_version = 1`, `revision`, `published_at`, and `author`. Choose a unique revision label for each changed catalog and update the publication timestamp. |
| `[[suite]]` | `id`, `title`, and `packages`, an array of package IDs. A package can belong to multiple suites. A suite can be empty. |
| `[[package]]` | `id`, `title`, and `kind`. `installer` is the kind the Windows client recognizes for installation actions, subject to its installer and capability rules. Other kinds can describe downloadable content such as `examples`. |
| `[[build]]` | `package`, `id`, `version`, `published_at`, and `about_url`. Optional `visibility` is `public` (default) or `targeted`. Optional `requires` is an array of capability IDs. |
| `[[build.artifact]]` | At least one per build, with a unique `platform` such as `windows-x64` or `any`, HTTPS `url`, `media_type` as `type/subtype`, nonnegative integer byte `size`, and `sha256` as 64 hexadecimal digits. |
| `[[build.extension]]` | Optional simple-text metadata with HTTPS `namespace`, XML local `name` without a prefix, and `text`. An optional `suite` must contain the build's package. Reserved Atom, XML, and updater namespaces are disallowed. A namespace and local name may recur only in distinct, explicitly scoped suite views. |
| `[[track]]` | `suite`, `package`, `name`, `platform`, and `build`, with optional `installation_id` for an override. The package must belong to the suite and the selected build must contain that exact artifact platform. |

The loader resolves references across files and rejects unknown fields,
duplicates, and dangling references. A later definition cannot override an
earlier one. It validates identifier grammar, HTTPS URLs, SHA-256 digests,
publication times, and XML text. Feed size is checked before activation.

## What a check-in can see

A suite groups packages. Each build has one or more artifacts with distinct
platform IDs. Track assignments label particular artifacts for a suite,
package, and platform. Public builds can remain available after their track
assignment changes. A UUID-specific assignment can override a public
assignment. A targeted build appears only when selected by a matching effective
assignment. Nil checks see the public view.

Track names are case-sensitive. They can use ASCII letters, digits,
underscores, and hyphens, with an optional final `*` and a maximum length of 64.
They start with a letter or digit. The Windows Client treats labels ending in
`*` as temporary. The [wire protocol](protocol.md) describes feed entries and
how the Client consumes them.

## Staging and production publication

A catalog revision is a complete snapshot. A Staging VPS can validate a
candidate with independent catalog and census state. The checks compare the
resulting feed, track assignments, and new or changed artifact contents with
the intended release. The versioned downloads and information pages are served
by the primary HTTPS server. Their URLs, sizes, and SHA-256 digests agree with
the catalog before the production VPS activates it.

The [catalog publisher](../tools/publish_catalog.py) exports the TOML fragments
from a selected Git commit and records that commit alongside the exported
catalog. It validates the candidate, prepares the inactive slot, and switches
the selection atomically. It requests a live reload and checks the active
revision. If activation fails, it restores the previous selection and verifies
the running revision. The source commit identifies the exported files. The
catalog revision identifies the snapshot returned in the feed.

## Live reload

On reload, a separate loader reads and validates a complete candidate while
the daemon continues serving the active catalog. The worker registers the
candidate's suite IDs and changes the active snapshot between checks.
Completed responses keep their existing bytes. Queued checks see the snapshot
active when their job begins.

The daemon rejects a changed snapshot that reuses the active revision and
rejects changes to a retained build's version, publication time, or an existing
artifact binding. Failed validation, activation errors, and reload timeouts
leave the active snapshot in place. The loopback health endpoint reports the
active revision and reload outcome.

See [catalog assembly](../src/catalog.cpp), [feed generation](../src/atom.cpp),
and [activation](../src/processor.cpp).
