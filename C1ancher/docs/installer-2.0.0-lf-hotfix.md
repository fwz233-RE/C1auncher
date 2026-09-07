# Installer 2.0.0: process-probe CRLF hotfix (2026-09-07)

## Root cause

`RunningProcessProbe.Command` was a multiline C# raw-string constant. C# preserves source-file line endings, so a Windows CRLF checkout embedded 29 CRLF sequences into the actual compiled command. `AdbClient.ShellAsync` sent those bytes unchanged. Device BusyBox rejected the loop with `syntax error: unexpected word (expecting "do")` at the before-removal process gate.

Uploaded `.sh` syntax checks did not cover this dynamically generated command. Earlier Python tests read C# source with `Path.read_text()`, which silently normalized CRLF to LF and masked the production failure. Fake ADB tests returned modeled snapshots without parsing shell syntax.

## Minimal fix

Keep the read-only shell logic and all identity/security checks unchanged. Retain the raw template as `CommandSource`, but initialize the outgoing `Command` with `ReplaceLineEndings("\n")`. Normalize only this fixed shell template, not arbitrary shell data or signed files.

Recompile the Windows EXE and assemble a complete bundle. Product metadata identifies `2.0.0+installer-lf-fix-20260907`. Core version remains 2.0.0 sequence 11, manifest SHA-256 `01d40bb99d9fc2cd72e2f13a480d8ed3f5bb86e43729033136a6c11bcc588173`; enrollment, signatures, keys, and all payload files are unchanged.

## Regression coverage

- Assert the actual compiled command and outgoing ADB argument contain no carriage returns.
- Preserve the exit wrapper and stop before removal/success on shell syntax failure.
- Python shell tests now read bytes from the Windows-compiled test assembly, without universal-newline conversion, rather than extracting source text.
- Reproduce the original CRLF failure on the pinned production BusyBox; verify the corrected command and wrapper parse successfully.
- Exercise private process fixtures including forks, deleted images, hash failures and process identity races.
- Exercise native `/proc` trees through the production C# snapshot analyzer.
- Explicit `--verify-running-process-readonly` test entry uses the real production ADB client and analyzer twice, without invoking installation or modifying the device.

## Device observation

On the development device (serial redacted), the corrected production probe accepted the same main PID 1132 and launcher PID 1131 twice, with hashes matching signed sequence 11. The installed core was already confirmed. No installation retry, removal, suspend, or reboot was performed during this fix. A full user-driven retry remains separate from this read-only acceptance.

Historical release snapshots are retained as build history; use the updated canonical installer bundle for retry.
