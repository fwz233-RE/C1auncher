# Package transport cooldown and retries

## Actual server policy

The checked local server sources are `C1ancher-server/config/repository.example.json`,
`internal/repository/http.go`, and `internal/repository/limits.go`. The production
configuration preparation script sets the same resource defaults:

- Two active large-file downloads and twenty queued requests.
- A shared 300,000-byte/second budget for application and core downloads, including
  Range responses.
- A maximum queue wait of 120 seconds; a full queue or expired queue wait returns
  HTTP 503 with `Retry-After: 5`.
- Small index/signature metadata does not occupy a large-file download slot.

These are concurrency, queue, and bandwidth limits, **not a requests-per-minute
quota**. No production endpoint or private deployment configuration is needed to
run the client regressions below. An external reverse proxy may impose additional
limits, which the client handles through HTTP responses rather than guessed quotas.

## Client behavior

`src/pkg/repo.c` keeps the existing public `c1pkg_fetch`, `c1pkg_repo_refresh`, and
progress callback signatures.

- Each endpoint gets at most four transfer attempts. HTTP 408, 429, 500, 502, 503,
  and 504 and transient curl DNS/connect/timeout/interrupted-transfer failures are
  retried automatically. Permanent HTTP errors and invalid ranges are not retried
  at that endpoint. Existing fixed-alternate rules still apply to ordinary endpoint
  failures; custom repositories do not acquire an implicit alternate.
- Metadata has a 45-second **network timeout per transfer attempt**. Packages have
  a 600-second timeout, a 15-second connection timeout, and a 180-second low-speed
  window, allowing the server's 120-second download queue. Server-directed waiting
  is separate from network timeouts; the old shared 45-second metadata-pair budget
  no longer truncates legitimate cooldowns.
- Numeric `Retry-After` and all three HTTP-date forms are accepted, including values
  greater than 60 seconds. A response's valid `Date` header supplies the reference
  clock for HTTP-date delays; otherwise the local wall clock is used. The maximum
  valid duplicate `Retry-After` wins. Intermediate redirect/proxy header blocks do
  not override the final response's headers.
- A successful response can set the next request's cooldown, including index to
  signature, signature to package, and subsequent user actions. Failed responses
  retain their cooldown even after retry exhaustion or cancellation. A monotonic
  deadline avoids early retries from wall-clock changes during a wait.
- Cooldown is scoped to a canonical repository base, rather than process-wide.
  Both the official HTTP domain and its fixed IP alias map to one key; paths
  belonging to different repositories remain separate. Lowercase authority,
  default HTTP/HTTPS ports and trailing slashes are normalized. Query strings,
  credentials, encoded/dot-segment paths and other ambiguous scope forms are
  rejected rather than allowed to acquire another key for the same endpoint.
- The signed refresh and cache-loading APIs bind their repository internally.
  A GUI parent that receives an index from a forked worker calls the pure
  `c1pkg_repo_bind_transport(config)` on initialization and configuration changes.
  It performs no filesystem operations or network waiting. It identifies the
  repository for subsequent package URLs, including nested archive directories.
  Without a bound hint, legacy direct fetches use the longest registered prefix,
  then the URL's parent directory; explicit binding disambiguates nested repos.
- Without a usable `Retry-After`, transient failures use 5, 10, and 20 seconds
  between attempts, retaining 30 seconds after final exhaustion. Other responses
  use a one-second client pacing interval. Zero/past-date hints also get that
  one-second minimum to avoid tight retry loops. These are bounded client defaults,
  not claims about a server request quota.
- A single automatic wait can be at most one hour. Longer or overflowing hints
  cause an explicit `EAGAIN` error **without an early request**. Their cooldown
  remains recorded; a subsequent action in the same process cannot bypass it.
  This deliberately trades automatic completion of extremely long waits for a
  finite, explicit bound rather than silently clamping a server's minimum delay.
- Exhausted HTTP 429/503 responses never trigger the fixed-IP alternate, which may
  be the same server. Any other allowed alternate request must first observe the
  shared cooldown. Thus switching from the domain to its IP cannot evade a delay.
- The existing progress callback receives a Chinese countdown once per displayed
  second and cancellation checks approximately every 100 milliseconds. A callback
  cancellation returns `ECANCELED`; cleanup preserves that value. GUI integration
  should render these messages and keep servicing cancellation input.
- Interrupted metadata restarts from byte zero. Package retries retain only this
  invocation's unpublished, validated prefix, validate final `Content-Range`, and
  discard ignored/stale ranges before restarting. Size ceilings and subsequent
  complete-package signature/hash verification remain mandatory.

A refresh may retry a complete signed index/signature pair up to three times for
publication races. The pairs stay on one endpoint and every constituent request
observes the same cooldown. Successful downloads return immediately; the wait is
shown when another request actually needs to start, not after an otherwise
finished operation.

## Scope and limitations

Cooldown is now stored separately from signed index state, under the existing
private `C1PKG_STATE_ROOT/cooldown/` directory. A short registry lock assigns at
most 32 immutable repository slots. Each slot has a private regular state file
and an independent process lock; at rest there are at most 65 files and less
than 64 KiB of data, plus at most one fixed temporary file per active slot during
atomic writes. Scans are bounded to these 32 known filenames. The implementation
never evicts a live cooldown to admit another repository: capacity exhaustion
fails closed and requires deliberate offline maintenance of obsolete slots.

The same-repository lock spans request admission, transfer and response-state
persistence. Another refresh, parent-process install or reopened GUI cannot
start that repository's request until the prior response is durably recorded.
Different repository slots can transfer concurrently; the registry is not held
across networking or countdowns. Nonblocking locks are polled with the existing
cancellable progress callback, only inside network operations, never navigation.

Records contain a boot ID, monotonic write time, remaining duration, deadline,
canonical scope and a SHA-512 corruption checksum (not a trust signature). They
are written to a fixed private temporary file, fsynced, renamed atomically, and
the directory is fsynced. Directories must be owned by the effective user and
0700; locks/state are 0600 regular files with a single link, opened no-follow.
Malformed size/schema/checksum, missing members of an assigned state/lock pair,
unsafe files, missing boot ID, failed monotonic clock, same-boot clock rollback,
I/O failure or numeric overflow stop requests
conservatively. A damaged slot cannot safely be attributed to a repository, so
registry validation fails closed rather than guessing its scope or deleting it.
The highest signed sequence and verified index files are never modified by this
cooldown layer.

Same-boot process restarts use the shared monotonic deadline and do not consult
the wall clock. After reboot, the remaining interval from the last durable
record is restarted in full on the new boot's monotonic clock and this conversion
is persisted once. Repeated GUI starts do not restart it. RTC adjustments therefore
cannot shorten a saved interval; reboot may conservatively over-wait. Saturated
deadlines never become zero or wrap. HTTP-date parsing still uses the response
Date reference when available, as before.

Before sending each request, the client persists a separate in-flight marker
while preserving its known Retry-After deadline. The normal response path clears
the marker and saves the later of the existing deadline and the new response's
Retry-After/backoff deadline, including callback-driven cancellation. If the
process is forcibly killed or power fails mid-request, the next holder of the
repository lock consumes that marker into a **one-time 60-second recovery wait**,
using the later of that recovery deadline and the saved server deadline. The
conversion is persisted before waiting: reopening the GUI or cancelling the
countdown does not restart or remove it. After reboot, the saved server interval
is conservatively rebased as above before applying recovery. An interrupted
ordinary request therefore does not permanently disable a repository.

The 60-second delay is a bounded client recovery policy, not a guarantee about
an arbitrary Retry-After in a response that was lost before durable recording.
Already saved server delays are never shortened, even if they exceed 60 seconds
or the automatic-wait limit. Corrupt records, unsafe storage and true arithmetic
overflow remain independent fail-closed errors. Legacy unbounded fence records
cannot be distinguished from saturated server delays and remain fail-closed;
this does not automatically reset or migrate them. Recovery from such errors
requires diagnosis without touching signed cache/sequence state. Graceful GUI
cancellation should still use the existing progress/cancel channel so response
cleanup can complete.

This coordinates c1pkg repository transport, not independent updater binaries or
non-package services. The server remains authoritative for actual quotas, global
concurrency and bandwidth. No new requests-per-minute quota is inferred.

Only the final response block is used for retry/cooldown policy; curl continues to
follow permitted redirects under the existing redirect count/protocol bounds.
The client does not interpret nonstandard `X-RateLimit-*` headers or infer a quota
from them. A server HTTP-date without a valid `Date` header depends on the device's
wall clock. Real device rendering/input responsiveness and production reverse
proxy policy require separate deployment validation.

## Local regression command

From the repository root on Linux or WSL:

```sh
python3 tests/test_pkg_http.py --build-dir build/transport-retry --repo-regression
python3 tests/test_pkg_cooldown.py
```

The test runner compiles the production transport with warnings as errors and
uses loopback-only HTTP fixtures plus a test-only curl URL rewrite. Cache tests
get a unique temporary state root; no device state or production services are used.
`tests/pkg_transport_clock.h` injects a deterministic clock only into the source
included by the test executables. Child process handling still uses real polling,
and real cancellation latency is tested separately. Production code contains no
environment-variable clock override or public testing API.

Coverage includes 75-second numeric and HTTP-date delays, three date formats,
server/device clock skew, duplicate and invalid headers, excessive numeric values,
redirect header isolation, signed metadata success-to-signature cooldowns,
package success-to-next-request cooldowns, exponential headerless backoff,
next-action cooldown retention after cancellation/exhaustion, fixed-IP bypass
prevention, bounded retries, actual cancellation latency, interrupted metadata,
validated package resumes, byte ceilings, child reaping, and preserved I/O errno.

The durable cooldown suite adds 25 isolated checks: fork-child to parent state,
exec/reopened-process remaining time, actual loopback HTTP response persistence,
separate and nested repositories, official alias/default-port normalization,
no-I/O configuration binding, cancellation retention, same-scope contention and
fresh-state reload, other-scope concurrent progress, boot conversion, clock
rollback, overflow, one-time bounded crash recovery (including cancellation and
reboot), preservation of longer known deadlines, shorter-response rejection,
missing/corrupt/linked state, storage bounds, and signed-index/sequence sentinels.
On 2026-09-19 all 25 passed with undefined-behavior sanitization; all 20 existing
HTTP cases and the complete repository/cache suite passed too. Clang static
analysis emitted no diagnostics and the production repo module compiled with
MIPS32r2/o32 hard-float flags and warnings as errors.

GUI integration uses the parent-side pure `c1pkg_repo_bind_transport` call
described above; the GUI integrator reports wiring it at initialization and
before installation. No GUI files or Makefile were changed for this fix. The
existing package link already includes Ed25519's SHA-512 implementation, so no
new library/source link entry is needed; add `python3 tests/test_pkg_cooldown.py`
to the desired host-test target. The original HTTP runner now supplies its own
unique compile-time state root, because production transport correctly refuses
to fall back to process-local-only cooldown when persistent state is unavailable.
No production server was found or accessed for this change.
