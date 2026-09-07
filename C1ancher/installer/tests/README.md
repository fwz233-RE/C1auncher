# Offline installer C# regression tests

This .NET 8 console harness links `Payload.cs`, `Commands.cs`, and
`InstallerEngine.cs` directly and uses BouncyCastle.Cryptography 2.7.0.
Run `dotnet run --project installer/tests/Installer.OfflineTests.csproj -c Release -- --workspace D:\c1slim`.
Add `--validator-exe <published C1SlimInstaller.exe>` to test the actual EXE's
validation-only CLI without initializing the GUI or invoking ADB.

The offline NuGet.Config clears package sources; restore requires a populated
local package cache. Use `dotnet run`, not `dotnet test`.

Fixtures generate temporary Ed25519 keys, signed structural MIPS ELF fixtures
that cannot run, and inert ADB/publisher files. Device shell fixtures have real
`#!/bin/sh` headers (`#!/usr/bin/env bash` for `neofetch.upstream`) but are never
executed; the sourced `c1-config.conf` fixture intentionally has no shebang.
The signed `enrollment/device-core-enroll.sh` fixture deliberately has no final
LF, matching the valid production script's EOF form without changing its bytes.
Installation commands are
intercepted by FakeCommandRunner; unknown commands fail rather than pass.
Real CommandRunner tests launch only local test child processes for timeout,
cancellation, and redirected-output behavior. No real ADB/device/network service
is used. Factory startup scripts are read-only hash fixtures. Temporary payloads
and evidence are cleaned up; the synthetic device's ordinary empty lock file
may remain in local application data.

Coverage includes payload membership and hashes; signature/digest/ABI/trust
validation; snapshots; serial selection; LF, CRLF, and CRCRLF exit markers;
upload progress, failure, timeout, and no automatic retry; developer-tool copy
verification; device baselines; helper success markers; enrollment failures;
and direct factory removal only after signed-core and running-image checks.
The default workflow must never call backup-factory, restore-factory, or ADB pull,
create a factory backup directory, or require a backup to remove factory software.
The result and log must explicitly disclose no factory backup.

The device-shell format matrix covers all 11 executed/loaded scripts:
`device-setup.sh`; `usb/device-open-adb.sh`, `usb/S90usb.original`,
`usb/S90usb.open`; both `profile/*.sh`; all three `enrollment/*.sh`;
`accessories/neofetch` and `accessories/neofetch.upstream`. It also covers the
sourced `accessories/c1-config.conf`. Each input must be nonempty, strict UTF-8
without a BOM and contain no CR or NUL. Line breaks must use LF, but a final LF
is optional: valid signed shell source must remain byte-for-byte unchanged.
Scripts must start with `#!`; the sourced config is exempt from that header
requirement.

For every input, regressions independently inject CRLF, a UTF-8 BOM, embedded
bare CR, NUL, invalid UTF-8, and empty content. Each script
also has a missing-shebang case. Fixtures refresh outer hashes and applicable
layer manifests; enrollment mutations refresh script digests and re-sign the
bootstrap with the temporary test key. Assertions require a format diagnostic
naming the exact file, rather than an earlier hash/signature error. All 83
invalid cases also run through `InstallAsync` and require zero fake ADB calls
and no installation evidence. Validation is checked byte-for-byte to ensure
it changes neither payload nor signature/manifest bytes. Positive cases also
accept non-ASCII UTF-8 and missing final LF. Snapshot regressions verify exact
preservation of every payload, manifest and signature byte with no final LF.
The original USB baseline without final LF passes format checks but still fails
its unchanged pinned digest, as required.
`Payload.DeviceShellScripts` exposes the fixed read-only script list, and
`Payload.DeviceShellConfig` identifies the sourced configuration.

The simulated installation verifies all eight uploaded `.sh`/`usb/S90usb.open`
syntax checks occur in manifest order after upload/hash verification and before
`preflight`. The eight syntax checks remain contiguous, but permission preparation
and 13 enrollment hash checks now intentionally intervene before `preflight`.
Each of the eight syntax checks is independently failed to prove
that no helper (`preflight`, `prepare`, accessories or removal), USB installation,
core enrollment, reboot or success evidence follows the failure.

## Automatic suspend preference fixtures

`python3 -B tests/test_auto_suspend_defaults.py -v` runs the actual suspend
capability and preference functions from the legacy helper in private temporary
roots, and requires the EXE helper and PowerShell host predicate to remain
identical. Cases cover the exact CPU machine identifier, battery platform node,
parent `gpio_keys` wakeup path (not nonexistent input-event wakeup nodes), real
read/write access checks with dropped root privileges, absent/stale probe files,
fresh defaults, explicit overrides, preserved marker contents, unsafe paths and
read-only verification. When host PowerShell is available, only the parameter and
guard prefix is executed in memory to check option routing and mutual exclusion;
the installer body, ADB and device commands never execute. The complete
PowerShell source is also parsed in memory without execution.

`python3 -B tests/test_installer_device.py -v` also exercises preparation in the
full private helper fixture. The corresponding `test_installer_busybox.py` cases
use the reviewed device BusyBox under emulation. These tests establish installer
policy and shell compatibility, not physical suspend or USB resume acceptance.
The documented release gate in `installer/BUNDLE-MAINTAINER.md` still applies.

`python3 -B tests/test_default_app_suspend.py -v` additionally overrides capability
explicitly to test policy independently of the host hardware. It checks default
argument dispatch, first installs, unsupported upgrades, marker metadata
preservation, explicit overrides and non-mutating verification.

## Explicit GUI completion preference policy

The GUI's confirmed installation overrides historical disable markers only after
signed enrollment verification, core start, and two matching running-process
snapshots. Both `enable-suspend <manifest-sha256> <c1pkg-sha256>` and
`verify-suspend <manifest-sha256> <c1pkg-sha256>` are bound to the immutable
host payload. The helper requires a signed current generation with those hashes,
a strictly `confirmed` state matching its sequence/version/security epoch, and
a running bootstrap before executing the physical current `c1pkg` artifact.
Legacy compatibility wrappers are never used. Preparation, enrollment and normal
core updates do not invoke this override.

`SuspendAcceptanceTests.cs` checks immediate and final acceptance, old disabled
preferences, fake reboot persistence, forged/missing/duplicate helper success,
and failures before or after removal. The Python device fixtures execute the
production helper with an inert `c1pkg` and verifier: actual marker removal,
exact status bytes, strict confirmation, expected-hash mismatch, malformed
paths, unsupported hardware, command errors, sync failure and preference races.
Unsupported hardware leaves or creates a disable marker and rejects success;
verification never re-enables a preference that was disabled later. Three new
BusyBox cases repeat positive/old-marker and trust/state failures using the
reviewed MIPS BusyBox via QEMU. These tests never suspend or reboot real hardware.

## Enrollment metadata preparation

`InstallerEngine.EnrollmentMetadataCommand(remote)` is the actual production
command constructor, linked into this test assembly. It accepts only the exact
`/storage/c1-installer-` prefix plus 32 lowercase hexadecimal digits (no trailing
newline, suffix, arbitrary UI path or stored enrollment location). Its private
fixed allowlists cover `enrollment`, `enrollment/release`,
`enrollment/release/artifacts` and exactly the 13 signed enrollment members.
After all staging uploads and eight `sh -n` checks, it checks every directory
is a non-symlink directory and every file is a regular non-symlink with link
count 1, before the first `chown`/`chmod`. It sets directories to 700, files to
600 and owners/groups to numeric 0:0, with no recursion. Every operation is
joined by `&&`; each `stat` assignment must itself succeed before its output is
compared. Final assertions check each mode, numeric owner and numeric group.
The engine then rehashes all 13 files against the payload before any helper,
USB configuration or enrollment execution. The later two-script `chmod 700`
also uses `&&` before executing the enrollment entry script.

For a Linux integration test to execute precisely this command in a private
fixture, first build the harness, then run
`dotnet installer/tests/bin/Release/net8.0/Installer.OfflineTests.dll --print-enrollment-metadata-command /storage/c1-installer-00000000000000000000000000000000`.
This branch prints only the real constructor's command to stdout and exits;
it performs no fixture discovery, signing, ADB call or installation. Invalid
arguments produce stderr and exit code 2, with no stdout. The caller can replace
that exact fixed staging prefix with its private temporary directory and run
the returned command there. The constructor itself never accepts temporary or
historical enrollment paths. Run Linux tests under an isolated environment
with the ownership privileges needed for numeric root ownership; never point
them at a device or a real `/storage` tree.

The strict fake starts uploaded directories at 777 and files at 666, both owned
by root, and independently checks the entire command's fixed paths, protective
expressions and ordering. Configuration and enrollment require completed
metadata preparation and all 13 subsequent hashes. Tests cover each directory
and file with symlinks, wrong types and missing entries; each file with hard
links, FIFOs, sockets and device nodes; every internal command failure position
(including all `chown`, `chmod`, and `stat` operations); wrong final mode, owner
or group on every entry; and a post-preparation hash mismatch on each file.
Precheck rejection must leave the metadata mutation count at zero. All these
failures must block helpers, USB configuration, enrollment, removal, reboot and
success evidence. One success test adds an inert upload to the minimal 30-file
fixture to exercise the observed 31-file staging package. The existing complete
pre-removal failure sweep still discovers command positions dynamically,
including the new metadata command and 13 hash checks.

Final permission-fix release run (2026-09-06, including the new published EXE):
**515 passed, 0 failed, 0 skipped**, in 99.83 seconds. The sweeps cover all 122
internal metadata operation failure positions and all 166 pre-removal fake
command failure positions. The command uses fixed-list shell loops: 1,319
bytes for the all-zero staging root, with an explicit limit and a regression
check that the complete ADB shell request remains below 4 KiB.

`tests/test_installer_enrollment_metadata.py` additionally runs the exported
production command in private WSL temporary directories with actual root-owned
files. Eight integration cases cover the observed 666 rejection, 600/700 repair,
unchanged production bootstrap/release signature validation through the native
production verifier, tampered signatures, links/special files, ownership and
failed permission operations. No ADB or device installation is performed.

A successful fake install determines every pre-removal command position, then
fault injection proves no failure at any such position reaches deletion, reboot,
or success evidence. These host tests do not execute the device helper, test
physical USB or reboot behavior, or undo an already requested deletion. The
separate Python helper tests use a private temporary root; Windows Forms UI
smoke tests are in `installer/tests/ui/`. Physical-device acceptance still
requires an operator-controlled installation.

## BusyBox and completion gates

`tests/test_installer_busybox.py` uses the captured device BusyBox 1.36.1
under `qemu-mipsel-static`, with fixed test input SHA-256 verification. The
capture was obtained read-only; the test does not contact the device. All
mutable paths, mounts and service effects are private fixtures. It reproduces
the unsupported GNU `find -links` predicate and the lost nonfinal-batch error.
It runs the replacement physical type scan and per-file `! -exec ... ; -print
-quit` rejection predicate with actual target applets, without relying on
propagation of child exit codes by find.
Failures must precede deletion, and complete cleanup/verify/retry must preserve
user files, ordinary apps and historical backups. Verifier fixtures in this
suite model the contract, not cryptographic proof.

`tests/test_installer_startup.py` executes only the production readiness
functions with virtual time and a fake background launch. It covers delayed
startup, already-running idempotence, timeout, writable-root refusal, launch
failure and verify-only waiting. `tests/test_installer_device.py` additionally
checks `start-core` is restricted to validated enrollment and cannot remove
factory files or change core history.

Host completion regressions cover the newly uploaded verification script,
validated start before the running-image gate, reboot return status and boot
identity, post-removal/reboot verification failures and final staging cleanup.
Success evidence and success logging are committed only after cleanup succeeds.
Reboot-enabled tests use fake ADB only; they never reboot a real device.
