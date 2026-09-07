# C1 core 2.0.0 integration

## Scope

This release integrates all C1ancher core source changes present in the working tree at the 2026-09-07 integration snapshot, rather than only the sleep/USB subset shipped as 1.3.7.

- Homepage: saved Wi-Fi labeling and prioritization, direct saved-profile connection without duplicate writes, and eligible post-scan automatic connection to the strongest visible saved network.
- Package manager: installed-library start page, direct launch, name/ID prefix selection, exact-match ranking, burst-input parsing, unmatched-prefix launch protection, backspace/timeout handling, and Space refresh.
- Supervision: retain the already-shipped bounded adopted-child reaping repair while the homepage remains healthy. Merge its process regression back into the primary working tree.
- Power: retain the signed 1.3.7 deep-sleep/USB behavior. Manual USB replug after wake is accepted; no forced full USB restart or Wi-Fi-triggered USB recovery.
- Kernel compatibility: enumerate candidate process IDs through `/proc` and use `waitpid` to prove direct-child ownership before signaling. Do not depend on `/proc/PID/task/TID/children`, which is absent on the shipping device kernel. Apply the same safe cleanup to launcher/terminal and updater logging supervision; preserve the primary child's exit status.
- Core updater, trust root, health confirmation, rollback protocol, and capability versions remain compatible with 1.3.7. The displayed product version is 2.0.0; bootstrap/updater capabilities remain 1.1.0 and the security epoch remains 1.
- GUI installation: after the delivered signed generation is confirmed and hardware support is established, explicitly enable and verify automatic suspend before reporting installation success. This is an installer completion policy, not a change to the generic core update/enrollment preference-preservation contract.

## Delivery gates

Use a clean committed source snapshot and the existing production Ed25519 key. Build all four core components, run host/lifecycle/installer regressions, create the signed release and complete enrollment, then assemble the rebuilt Windows GUI with that exact enrollment.

The same signed manifest and four component hashes must be verified across the local release, packaged enrollment, server channels and USB device. Preserve historical releases and back up any replaced installer directory. Use the normal prepare-local/activation/confirmation flow; never replace current binaries or generation links directly.

No deliberate device suspend, reboot, physical-key test or cable test is part of this rollout. Offline tests and confirmed installation do not establish battery life or a new full automatic-suspend acceptance result.

Final sequence, source identity, checksums, publication state and device evidence are recorded in the delivery checkpoint outside the source tree after validation.
