# Installer UI layout smoke test

Run on Windows with .NET SDK 8. The harness links the real form and uses
`MainForm(offlinePreview: true)`, so it never starts the ADB scanner or accesses a
device. It opens an off-screen test window, verifies control bounds, text sizing,
minimum device selector width, and the disabled template install button, then
saves screenshots and closes the window.

From the C1ancher project:

- `dotnet run --project installer/tests/ui/Installer.UiSmoke.csproj -c Release -- build/ui-preview`
- `dotnet run --project installer/tests/ui/Installer.UiSmoke.csproj -c Release -- build/ui-preview-96 --dpi-unaware`

The first run uses the monitor's real per-monitor DPI (192 on the verification
machine). The second uses a DPI-unaware 96-DPI coordinate context to check the
100% layout. This is not a physical monitor-switching test. Each runs at logical
client sizes 1080×780, 900×740 and 1280×900. All six combinations passed on
2026-09-06. The separate 139-case offline installer regression suite also passed
against the newly published GUI EXE's validation-only entry point.
