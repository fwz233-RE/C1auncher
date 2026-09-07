# Core update operations

This runbook covers the independent C1 core trust chain. It does not use the application repository key, layout, sequence, or release format.

## Local lifecycle repairs (2026-09-06; not deployed)

The local implementation now uses a fixed `/etc/c1updater/recovery-verifier`, installed only by trusted enrollment or maintenance. It verifies signed generation contents before executing the verified updater inode. Normal four-component core releases cannot replace this helper or `/etc/app_daemon`.

Existing enrolled devices require an explicitly authorized `install-core-enrollment.ps1 -Action Maintenance` operation before relying on the new bootstrap contract. Maintenance checks the bundle's raw public-key hash against the existing device key before executing bundle code; local validation also requires the PEM and raw public keys to be identical. Initial enrollment still requires obtaining the bundle through a trusted provisioning process. Maintenance retains the existing key, generation pointers, transaction state, and pending-boot counter.

Bootstrap/updater capability versions are now `1.1.0`, independent of the displayed core release version. The protected `bootstrap.version` record binds the capability version to the installed bootstrap SHA-256. Signed releases requiring only the legacy `1.0.0` contract remain eligible for safe rollback; new minimum-version requirements are enforced before activation and boot.

Lock contention returns a retriable status rather than a corruption signal. The bootstrap stays the parent, tries the alternate updater after bounded ordinary failures, and handles statuses 71 (fatal), 72 (slot transition), and 75 (transient) consistently. Pending candidates may start on at most three distinct kernel boot IDs; the fourth boot rolls back before starting that candidate. Confirmation requires both the digest-bound ready marker and continuous launcher-observed UI heartbeat evidence across the 30-second observation window.

Local regressions are available through `make BUILD_DIR=build/lifecycle-fixes lifecycle-test`. Runtime logging uses the fixed helper to cap each file at 64 KiB with four rotated files; write failures disable persistence while continuing to drain output and preserve child exit handling. The Windows public-key validation test is `powershell -NoProfile -File tests/test_enrollment_host.ps1`. These tests use isolated paths and fixtures; they do not constitute physical power-loss, reboot, or maintenance acceptance. This change does not authorize deployment, stable promotion, or production signing.

## Security boundaries

- Keep the Ed25519 private key outside every repository and build output. Back it up offline before first production use.
- Install only the matching raw 32-byte public key on devices at `/etc/c1updater/core.ed25519.pub`.
- Install only the matching PEM public key on the server at `/srv/c1core/trust/core.ed25519.pem`.
- Never put either public key in a normal core release. An enrollment bundle may carry the public key because enrollment establishes the trust root.
- Never copy the server address file into a build directory or log its content. Publication reads it only after local release validation.
- Do not modify `/usr/data/c1/disable-auto-suspend` during enrollment, update, rollback, or recovery.

## Production gates

Do not enroll a production device or publish a production channel until all gates are complete:

1. The intended source tree is committed and `build-core-release.ps1` succeeds without `-AllowDirtySource`.
2. The production private key is stored and backed up outside the repositories.
3. The fixed server and device public keys have been compared byte-for-byte with the key derived from the production private key.
4. `setup-core-repo.sh` has passed in an isolated root environment with the production Caddy version.
5. HTTP GET/HEAD succeeds and write methods return 403 for both repositories.
6. The representative device has a fresh `/etc/app_daemon`, `/usr/data`, and `/storage` backup.
7. The fault matrix below has passed on an enrolled development device.

A failed gate stops rollout. Do not bypass source cleanliness with `-AllowDirtySource` in production.

## Build a deterministic core release

Run `scripts/build-core-release.ps1` from the `C1ancher` repository with an explicit sequence, security epoch, source date epoch, and external key path. Keep the generated PEM/raw public-key outputs outside the release directory.

The release must contain exactly:

- `manifest.v1`
- `manifest.v1.sig`
- `artifacts/C1ancher`
- `artifacts/c1pkg`
- `artifacts/C1ancher-launcher`
- `artifacts/c1updater`

Validate with `scripts/validate-core-release.sh` using the fixed PEM public key. Record sequence, version, source revision, source date epoch, manifest SHA-256, and release ID in the change record. Do not record credentials or private-key paths.

## Install the server repository

Run `C1ancher-server/scripts/setup-core-repo.sh` as root with the fixed PEM public key, public server name, listen port, and a dedicated publisher account.

The script creates:

- root-controlled immutable releases and channels under `/srv/c1core`
- publisher-writable staging only
- root-controlled disabled-release markers
- root-owned validator and activation helper under `/usr/local/libexec/c1core`
- a narrowly scoped sudo rule for the activation helper
- GET/HEAD-only Caddy routes for `/c1/core/v1/{canary,stable}`

After setup, verify ownership and modes. The publisher must not be able to write `releases`, `channels`, `disabled`, `trust`, or the activation helpers.

## Publish, promote, pause, and disable

Publish a locally validated release with `C1ancher-server/scripts/publish-core-release.ps1`. A successful publication moves `canary` atomically and leaves `stable` unchanged.

Promote only the active canary with `promote-core-release.ps1`.

Emergency controls use `manage-core-channel.ps1`:

- `-PauseChannel canary` removes the canary pointer atomically.
- `-PauseChannel stable` removes the stable pointer atomically.
- `-DisableReleaseId <release-id>` writes an immutable disabled marker and withdraws any matching canary/stable pointers under the publication lock.

A disabled release cannot be published or promoted again. Releases remain on disk for audit and recovery; disabling changes channel availability, not historical bytes.

## Enroll devices

Build an enrollment bundle with `scripts/build-core-enrollment.ps1`. Supply the signed core release, the same external core private key, and the exact approved SHA-256 values for `/etc/app_daemon` baselines.

### ADB or on-site enrollment

Use `scripts/install-core-enrollment.ps1 -Action Install -BundleDirectory <bundle> -Reboot`.

The installer:

1. requires exactly one root ADB device and a read-only root mount;
2. validates the bundle locally before executing any bundle code;
3. pushes every file separately and verifies device-side SHA-256;
4. revalidates the signed bootstrap and core manifests on-device;
5. retains the original startup script and legacy compatibility binaries in the historical recovery directories (not a backup of factory learning software);
6. installs root-owned updater A/B slots, the fixed raw public key and independent recovery verifier;
7. durably saves a temporary root-protected, hash-bound copy of the approved startup script, then atomically installs the real bootstrap and its capability record;
8. restores the root mount to read-only, then runs the unchanged `prepare-local` compatibility and signature checks;
9. uses `bootstrap-activate` to establish and confirm the first generation, verifies its full contents, and only then changes compatibility links;
10. durably commits the enrollment marker, removes the temporary startup authorization and script, and restores the root mount to read-only before reporting success.

The temporary startup path is authorized only for incomplete first enrollment;
a completed enrollment never returns to vendor startup. It does not copy
`/usr/bin/d261` or delete historical backups. A pending first activation can be
resumed by the authenticated enrollment flow; ordinary GUI preflight continues
to reject incomplete non-factory enrollment pending trusted recovery. Physical
power-loss and cold-boot behavior still require device acceptance.

Use `-Action Verify` for a non-destructive check. Use `-Action Uninstall` to restore the original daemon and compatibility binaries. Signed generations are retained for forensic recovery.

`install-default-app.ps1` accepts `-CoreEnrollmentBundle` to chain default-app installation and enrollment before one optional reboot.

### Application-repository enrollment

An enrollment bundle is also a complete application payload. Add it to an application catalog as:

- ID: `c1-core-bootstrap`
- version: `1.0.0`
- entry: `enroll.sh`
- payload directory: the immutable enrollment bundle

Build and sign with the existing application repository builder. The application signature authorizes delivery; the embedded core signature authorizes the exact bootstrap, updater, key, baseline set, and initial core release. Remove the package from the catalog after the enrolled coverage target is reached.

Unknown root-daemon hashes must fail closed and enter the manual recovery queue.

## Fault matrix

Run host tests before every release. They cover strict parsing, signature/key failures, links, path rejection, size overflow, rollback, same-sequence conflicts, transaction retry, first-generation activation, updater slot self-test, crash policy, request isolation, and interrupted state generations.

Run these destructive tests only on a backed-up development device with physical recovery access:

| Fault | Injection point | Required result |
|---|---|---|
| Network interruption | each manifest/signature/artifact download | current confirmed generation remains bootable; retry is bounded |
| Bad signature/key | before artifact download | no prepared generation and no pointer change |
| Artifact size/hash mismatch | staging verification | staging is rejected and current remains unchanged |
| `/storage` unavailable | before and during download | update fails without touching `/usr/data` pointers |
| `/usr/data` full | candidate copy | no partial committed generation; current remains unchanged |
| Read-only root | updater slot/bootstrap enrollment | enrollment aborts and restores root read-only |
| Power loss | after each file write, fsync, directory fsync, generation rename, state link rename, and core pointer rename | reboot selects either the old confirmed generation or the complete new generation, never mixed components |
| Candidate launcher crash | before ready and before confirmation | supervisor restores previous and records failed identity |
| Repeated C1ancher crash | five short runs | crash storm is detected and rollback/retained supervision follows policy |
| Corrupt inactive updater slot | before slot selection | current slot continues; failed self-test cannot replace a slot |
| Fatal active updater | after alternate slot is installed | root bootstrap switches once to the other valid slot |
| Both updater slots invalid | completed enrollment | fixed verifier tries signed previous/current generations; no vendor fallback |
| Confirmation timeout | candidate stays alive without valid health marker | candidate is not confirmed and previous is restored |
| Concurrent publish/promote | hold publication lock | exactly one operation succeeds; the other fails closed |
| Disabled release | publish/promote after disable | operation is rejected and no channel points to it |
| Repeated enrollment | after confirmed generation but before marker, and after marker | operation is idempotent and backups are unchanged |
| Enrollment uninstall | after a cold boot | original daemon/process chain is restored; auto-suspend marker is unchanged |

For every device test, capture only release ID, manifest digest, phase, monotonic timestamps, process counts, pointer targets, mount state, hashes, and result codes. Do not capture URLs containing parameters, keys, passwords, or the server address file.

## Canary rollout

1. Enroll one development device and reboot twice.
2. Publish the signed release to canary.
3. Confirm update success, health-confirmation time, boot count, rollback count, and updater-slot status.
4. Expand to a small physically recoverable batch.
5. Pause canary immediately if confirmation latency or rollback rate exceeds the release threshold.
6. Promote the exact active canary bytes to stable; do not rebuild or resign.
7. Expand stable in explicit batches with an observation window after each batch.
8. Disable the release and pause affected channels on evidence of a security or boot-safety defect.

## Disaster recovery

1. Stop automatic rollout by pausing canary and stable.
2. Preserve device state, core generation directories, state generations, enrollment log, and hashes before changing anything.
3. If the candidate is pending, run the known-good updater `rollback` command with the fixed state/core/key paths.
4. If the active updater is fatal, select the other verified slot in `/usr/data/c1/update/updater-slot` using a same-directory temporary file and rename.
5. If both slots fail, restore `/etc/app_daemon.c1-original` from the root copy or either writable-filesystem recovery copy, verifying SHA-256 before same-directory replacement.
6. Remount root read-only and verify the expected PID chain after restart.
7. Never delete current, previous, pending, disabled-for-investigation, or sole factory-recovery bytes.

Historical release cleanup is intentionally manual. Before deleting any server or device generation, resolve every channel/current/previous/pending pointer and prove that at least one confirmed generation plus the factory recovery path remains valid.