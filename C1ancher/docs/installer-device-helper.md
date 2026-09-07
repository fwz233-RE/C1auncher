# Device setup helper: Windows installer contract

`installer/device-setup.sh` is the root-shell companion to the Windows installer.
The default workflow directly removes the factory learning software after signed
core enrollment and host verification of the actual running home process. There
is no factory-backup option, backup creation, host backup pull, or restore command.
Historical backups are left untouched, as are ordinary applications and user data.
The signed core's independent recovery verifier and update recovery remain intact.
The signed `device-core-enroll.sh` installs the real bootstrap and its protected
hash-bound capability record before `prepare-local`. Initial enrollment keeps a
temporary, root-protected copy of the approved startup script (not the factory
learning application) so an interruption before signed activation does not leave
boot without an entry point. The bootstrap can use it only while the enrollment
marker is absent. Successful enrollment removes it before reporting completion;
a completed enrollment always uses signed core recovery. Historical recovery
copies are retained. Normal GUI preflight still rejects an incomplete non-factory
enrollment; recovery of that state requires trusted enrollment handling, not a
weakened preflight or repeated forced installation.

## Command and output protocol

Execute `/bin/sh REMOTE_HELPER COMMAND` for exactly one of:

- `preflight`: require UID 0, read-only root, read-write `/storage` and `/usr/data`,
  at least 64 MiB free on `/usr/data` and 16 MiB on `/storage`, safe critical paths,
  and recognized startup scripts.
- `prepare`: enable automatic deep suspend on a first install only when
  `auto_suspend_supported` recognizes `ingenic,halley6_v20`, the `mpenbatt`
  platform device, enabled `gpio_keys` wakeup, and readable/writable `mem` suspend.
  Create canonical `Pic`, `Music`, and `Book` media directories without replacing
  user contents. Preserve existing disable markers byte-for-byte, including
  legacy installer markers and empty files. Unsupported hardware gets a disable
  marker even on retries or upgrades; supported reinstalls preserve preferences
  during preparation only. Successful Windows installs subsequently override
  existing disable preferences with `enable-suspend` and verify the result.
  Hardware capability is not evidence of physical suspend/resume. After waking,
  manually unplugging and reconnecting the USB data cable is an accepted recovery
  step if USB/ADB does not return. Wi-Fi-triggered USB repair and a full USB
  restart fallback are outside the product requirement. Keep power and USB
  connected while installation, updates or file transfers are active.
- `accessories PAYLOAD_DIR`: verify the six-file accessory payload, install the
  vendored Neofetch package and PATH profile, and provision a missing wallpaper.
  The absolute payload directory uses ASCII letters, digits, `_`, `.`, `/`, and
  `-`, with no traversal components. This path restriction does not apply to
  filenames inside the factory directory being deleted.
- `start-core`: after preflight and signed-core validation, start an already
  enrolled core if stopped and wait at most 20 seconds for the bootstrap. It
  does not alter core generations, factory files, or removal markers. The host
  still verifies the actual matching home process before any removal.
- `enable-suspend MANIFEST_SHA256 C1PKG_SHA256`: after authenticated enrollment
  and host running-image checks, require both lowercase SHA-256 pins to match
  this installation's payload, a confirmed matching update state, and supported
  suspend hardware; invoke the verified current core's
  `c1pkg power enable`. Existing regular disable markers are deliberately removed
  by the core preference API, including empty and legacy installer markers.
  Persist and check the enabled preference; failures stop the installation before
  factory removal. Unsafe markers and untrusted cores are rejected.
- `verify-suspend MANIFEST_SHA256 C1PKG_SHA256`: independently validate the
  current core against the same payload pins and hardware, require
  an absent disable marker and the exact enabled `c1pkg power status` response.
  This action never re-enables a disabled preference. If hardware capability is
  lost, either suspend action creates a disabling marker if absent and fails
  closed; final verification is therefore allowed to disable for safety.
  The host runs it immediately
  after enabling and again at the end, after optional reboot acceptance.
- `remove-factory`: verify signed-core enrollment and recognized S80app, reject
  unsafe deletion boundaries, stop only the bootstrap and exact vendor processes,
  record the no-backup/removing markers, replace S80app, and directly delete only
  `/usr/bin/d261`. Restore read-only root before restarting `/etc/app_daemon`.
  No backup is created or checked. Valid marked partial deletions can be retried.
- `verify`: check signed-core enrollment, no-backup/removal markers, absent factory
  directory, replacement S80app, prepared/accessory markers, Neofetch hashes,
  PATH profile, running bootstrap, and read-only root. This general integrity
  action accepts a user-deleted wallpaper and later suspend preference changes;
  Windows installation additionally requires `verify-suspend` before success.

Success is exit status 0 and exactly one `C1SETUP_OK COMMAND` stdout line, emitted
only after cleanup succeeds. Errors have nonzero status and `C1SETUP_ERROR`
stderr diagnostics, including `command-failed:COMMAND:exit-N`. The host must
check both the exit status and unique success marker.

Host order: authenticate and upload payload; parse all eight uploaded installation
scripts with device `sh -n` before executing any of them; `preflight` and `prepare`; install
ADB support and accessories; install and verify the signed core using the fresh
authenticated upload, start it if stopped, and verify the actual running home
image; enable and independently verify deep suspend with `enable-suspend` and
`verify-suspend`; configure repositories and copy developer tools; run
`remove-factory` and `verify`; verify the actual running home and repositories
again; optionally reboot and repeat verification; finally run `verify-suspend`
again before staging cleanup and success recording. Core validation, suspend
activation, or initial switch-check failures stop before requesting removal.
Final switch-check failure also records no success and retains recovery staging.
Success evidence records `automatic_suspend=enabled` and
`automatic_suspend_verified=true`; this verifies a preference, not a physical
suspend/wake cycle. The installer does not flash a whole
device image, format storage, or alter bootloader partitions.

## Deletion safety and retry state

All fixed paths reject symlinks in every ancestor, traversal, and special files.
A filename-independent physical `find` scan of `/usr/bin/d261` rejects symlinks,
special files, and multiply hard-linked regular files. Nested mounts are rejected.
Chinese, spaces, and newline filenames are accepted without parsing pathname
lines or generating an inventory. The boundaries are checked again after stopping
the process chain and before opening root read-write.

The scan uses BusyBox-compatible `find ! -type d ! -type f -print -quit`
to detect unsafe types. Link checks use a per-file `! -exec sh -c ... ; -print
-quit` predicate: stat failure or a count other than `1` causes a printed path
and immediate stop. Only output presence is tested; filenames are never parsed.
It requires neither GNU find's unsupported `-links` nor child-exit propagation.
The target BusyBox was observed to lose an earlier `-exec {} +` batch failure,
so that otherwise faster implementation is deliberately excluded. Preflight
checks positive/negative predicate controls on the private lock flag before
USB/core changes. The regression suite runs the captured device BusyBox 1.36.1
in QEMU; these are isolated tests, not physical deletion or reboot acceptance.

State lives under `/usr/data/c1/installer/`: `prepared`, `wallpaper-handled`,
`accessories`, `accessories.sha256`, `no-factory-backup`, `removing`, and `removed`.
A previously recorded `restored` state is refused for review. A missing factory
tree or an already replaced S80app requires valid no-backup/removing markers;
unknown or corrupted state does not silently authorize deletion. No historical
backup or incomplete backup directory is changed.

A private `/usr/data/c1/installer.lock` directory serializes helper commands.
A stale lock is refused rather than stolen. Operators must first confirm no
installation is active before recovering a stale lock. The host also serializes
the separate core and repository scripts. Root enters and exits read-only; a
trap attempts to restore read-only on errors/signals. Failure to remount read-only
is an error, and the chain is never restarted while root remains writable.
Power loss and SIGKILL cannot run shell cleanup; root state then needs review.

The generated S80app only starts/TERM-stops `/etc/app_daemon`. Vendor `mpenMain`,
vendor fallback, broad `killall`, and reboot policy are absent. Vendor process
termination matches `/proc/PID/exe` beneath the exact factory directory.

Reviewed SHA-256 baselines from `firmware-analysis/system-rootfs`:

- S80app: `d35cdaa670636511c04d70b5e21b61e1938d2e93b33ae9379a01df1f340cafc2`.
- app_daemon: `ceb56ddf2ff3c10f7c4c2cd6216b298da1cea799ca7170322f8229d5e9af6ee7`.

The only alternate S80app accepted is this helper's exact generated script.
An alternate app_daemon requires enrollment marker hash bindings, bootstrap
version, recovery verifier, protected key, verified current release, and a
confirmed/idle update state. Arbitrary legacy startup shims are refused.

## Accessory payload

`PAYLOAD_DIR` contains six regular single-hard-link files and `SHA256SUMS`:
`neofetch`, `neofetch.upstream`, `c1-config.conf`, `c1-logo.txt`, `LICENSE.md`, and
`wallpaper.raw`. Neofetch shell files from `third_party/neofetch/` are normalized
from CRLF to LF by the Windows bundle assembler; logo/license/binary wallpaper
bytes are copied unchanged. Signed enrollment bytes are never normalized.
The wallpaper is exactly 5624 bytes, representing 296 columns
and 19 vertical strips of eight pixels. The manifest contains exactly six
LF-terminated lines with lowercase SHA-256, two spaces, and a fixed filename.
The host independently authenticates all payload bytes; this manifest is only
an integrity recheck, not an independent signature or trust anchor.

The wrapper requires `/bin/bash`, installs as `/usr/data/c1/bin/neofetch`, and
uses `/usr/data/c1/neofetch/` for its auxiliary files. The helper checks that the
existing `/etc/profile` sources `/etc/profile.d/*.sh`, then installs the known
`90-c1-path.sh` profile without replacing an unknown existing file.

Wallpaper is `/storage/mtp/Pic/wallpaper.raw`. Existing regular files are preserved.
If absent, migrate the legacy `/storage/mtp/pic/wallpaper.raw` or use the bundled
default. Re-running accessories repairs a missing wallpaper even on enrolled
devices. No lowercase picture directory or duplicate wallpaper is created.
The signed core independently provisions its embedded default when absent and
preserves existing files. The picture viewer does not select arbitrary images
as lock-screen wallpaper.

## Tests and acceptance boundary

Run `python3 tests/test_installer_device.py` on Linux or WSL. Tests copy the helper
to a private temporary root, rewrite all production data/rootfs/process/mount
paths, and stub UID, mount, process control, chown, sync, and free space. Production
has no alternate-root or runtime test bypass. Factory scripts are read-only hash
fixtures and are not executed. No ADB or real-device deployment is performed.

The 2026-09-07 default-enable change is source-only: existing EXE/payload bundles
have not been rebuilt or repacked for it. Delivery must identify and validate the
actual signed core bytes, including physical `mem` sleep, power-key wake and USB/ADB
reconnection with manual cable unplug/replug accepted. The assembler's `READY`
status is an offline integrity result, not physical acceptance. See
`installer/BUNDLE-MAINTAINER.md` for existing artifact paths and the new-directory
build/assembly steps; changing a shell source or outer checksum cannot update a
signed core.

Coverage includes default no-backup install, Unicode/space/newline filenames,
partial-deletion retry, historical-backup and user-data preservation, unknown
scripts, enrollment tampering, symlinks/hard links/special files, nested mounts,
free space, remount errors, state validation, payload verification, preferences,
and idempotency. These tests are not physical-device installation acceptance.
