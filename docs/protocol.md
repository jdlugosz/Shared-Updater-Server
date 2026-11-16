# Shared Updater HTTPS and Atom protocol

The update host serves two things over HTTPS. It proxies check-ins to the Shared Updater Server, and it serves the versioned packages linked from the Atom feed. The Windows updater selects and verifies downloads locally. An application and its installer integrate with the updater through the Windows client contract in the [Shared Updater Client integration guide](https://github.com/jdlugosz/Shared-Updater-Client/blob/main/INTEGRATION.md).

## Check for updates

```http
GET /updchk/00000000-0000-0000-0000-000000000000/launcher HTTP/1.1
Host: updates.example.invalid
Accept: application/atom+xml
Cache-Control: no-cache, no-store
```

- Use exactly two path segments after `/updchk/`. The installation identifier is a UUID in `8-4-4-4-12` hex form. The daemon treats all 16 bytes as opaque, accepts either hex case, and reserves the all-zero Nil UUID for anonymous checks. Use the whole UUID when targeting an installation. A machine tag embedded in a UUID is part of the identifier.
- A suite ID is 1–64 ASCII characters, begins with a letter, and continues with letters, digits, `_`, or `-`. The URL accepts either letter case. The feed returns the canonical lowercase suite ID. The `/updchk/` prefix is case-sensitive.
- To receive selected platforms, append `?platform=windows-x64-avx2&platform=windows-x64`. Each value is a lowercase catalog identifier (1–64 characters, starts with an ASCII letter or digit, and otherwise allows lowercase letters, digits, `_`, and `-`). The result contains the matching platforms and `any`. Query order carries no preference. With no query, the daemon returns all platforms. The Windows client uses the unfiltered request and applies its platform preference locally.
- Use the literal path and query with no percent escapes or fragment. The path and query together are limited to 256 bytes by default. The HTTP/1.1 request needs one nonempty `Host` header. Send the GET without a body, transfer encoding, or an `Expect` header.

| Result | Interpretation |
| --- | --- |
| `200` with `Content-Type: application/atom+xml; charset=utf-8` | Complete feed for the suite. A known suite can yield zero entries, including when the requested platforms have no eligible artifacts. |
| `400` | Malformed request, target, UUID, suite, or query, or a forbidden request body. |
| `404` | Unknown route or unknown suite. |
| `405` with `Allow: GET` | Unsupported method on a well-framed request. A HEAD response has no body. |
| `431` or connection closure | Header limit, idle timeout, or another failure at the HTTP connection level. |
| `503` with `Retry-After: 30` | Busy, queued request expired, or the daemon could not complete the check. Retry later. The proxy can return its own errors if the daemon is unavailable. |

Daemon responses carry `Cache-Control: no-store`. The Caddy example forwards the full request target and leaves retries to the Client. The backend binds to loopback and the example exposes only check-ins through the proxy. The check itself records an observation. The daemon renders the feed, appends the bounded contact log, and commits the census update before sending a success response. A lost response or a retry can increase traffic counts. The census groups identified activity by full UUID and suite and tracks Nil requests in separate daily counters. See the [recorded data and retention](settings.md#census-and-contact-log) and [database design](../DATABASE.md).

## Atom feed structure

Parse elements by namespace URI and local name. The examples use Atom's default namespace `http://www.w3.org/2005/Atom` and `upd` for `https://dlugosz.enterprises/ns/updater/1`. Namespace prefixes can vary. The feed is UTF-8 XML, with one entry for each eligible artifact of each build in the suite.

| Level | Elements |
| --- | --- |
| Feed | Atom `id`, `title`, `updated`, and `author/name`. Updater `suite` and `revision`. |
| Entry | Atom `id`, `title`, `updated`, `published`, and two links. Updater `package`, `kind`, `build`, `version`, `platform`, and `sha256`. |
| Entry links | `rel="alternate"` with `href` for the information page. `rel="enclosure"` with `href` for the artifact, `type` for its media type, and `length` for its decimal byte count. |
| Repeated entry fields | Updater `track` labels and `requires` capability IDs. |
| Optional entry metadata | Publisher-defined simple-text elements in their own namespaces. A catalog can restrict an extension to one suite. |

The server emits stable HTTPS IDs under `publisher_id_base`. The feed ID ends in `suites/<suite>` and the entry ID in `packages/<package>/builds/<build>/artifacts/<platform>`. These IDs are independent of the check-in UUID and current track assignment. The `updated` fields reflect the catalog publication time and `published` reflects the build publication time. Timestamps are UTC with `Z` and optionally one to nine fractional digits. The catalog revision identifies the published snapshot. The `(package, build, platform)` tuple identifies an artifact. The SHA-256 field contains exactly 64 hexadecimal digits.

For a complete example, see the [Windows parser's Atom fixture](https://github.com/jdlugosz/Shared-Updater-Client/blob/main/tests/Updater.Tests/Fixtures/launcher.atom) or emit the feed for the [server example catalog](../examples/catalog) locally.

## How offers are selected

The catalog marks builds public or targeted. A public artifact can appear without a track label, including when an older version is retained for manual download or recovery. The daemon starts with public track assignments and overlays assignments for the requesting UUID by `(suite, package, platform, track name)`. A targeted artifact appears only when an effective matching assignment selects it. A replaced public artifact can still appear with that label removed. A check with Nil sees the public catalog. Treat targeted offerings and links as non-confidential. The UUID establishes which offers to display and supplies a census identifier. The download host serves the linked files independently.

Track labels compare case-sensitively. Each label starts with an ASCII letter or digit, can then contain letters, digits, `_`, or `-`, and may have one trailing `*` within its 64-character limit. The client displays `*` labels only while offered and can remember other labels across checks. Client policy treats the exact lowercase `release` as the default and lets `beta` also consider a newer `release`. For the registered primary package, Windows prefers its native OS platform, then `any` if no native artifact is tagged for that track. Automatic version comparison uses one to four dotted nonnegative integers, with missing components read as zero. See the [Windows integration guide](https://github.com/jdlugosz/Shared-Updater-Client/blob/main/INTEGRATION.md) for scope, cadence, and the rules that permit Setup to start.

## Client validation and download

The Windows client checks for status `200`, the Atom media type, the expected suite, the required XML fields, and unique entry IDs and `(package, build, platform)` combinations. Each track label selects at most one build for a given package and platform. The client limits feeds to 512 KiB even if the daemon's configured output limit is larger. It prohibits DTDs, accepts absolute HTTPS links and identifiers without embedded credentials, and ignores optional extension elements it does not use. Declare capabilities through `requires` when installation needs client-side support. The client only considers such offers for automatic updates when the relevant installed client code supports them.

The Windows transport has a 35-second overall check deadline and a 10-second connection timeout. It follows no redirects and sends no cookies. Serve enclosures directly over HTTPS. The client downloads to a temporary file, verifies the complete byte count and SHA-256, and moves the file into place only after verification. Publisher signatures provide additional verification for the supported Launcher and Shared Updater Setup paths. See [catalog inputs and publication](catalog.md) for the records that produce these offers.

## Implementation

See [request routing](../src/request.cpp), [HTTP responses](../src/http_server.cpp), [Atom generation](../src/atom.cpp), [check persistence](../src/processor.cpp), and the Windows client's [parser](https://github.com/jdlugosz/Shared-Updater-Client/blob/main/src/Updater/AtomFeed.cs), [transport](https://github.com/jdlugosz/Shared-Updater-Client/blob/main/src/Updater/UpdateTransport.cs), and [selection policy](https://github.com/jdlugosz/Shared-Updater-Client/blob/main/src/Updater/UpdatePolicy.cs). [Atom specification (RFC 4287)](https://www.rfc-editor.org/rfc/rfc4287.html)
