# Wi-Fi interface and connection redesign

Local development changes only. No version bump, device installation, signing,
or repository publication is performed by this work.

## E-paper interface

The actual 296 x 152 monochrome renderer now separates the status header,
two actions (scan and switch off), three network rows, a result line, and fixed
key help. Selection is a solid black row with white text. There are no animated
spinners, fake percentage bars, gradients, or timed scanning redraws.

Network names use the existing bundled 16-pixel UTF-8 bitmap font from
`src/pkg/text.c`, including Chinese names. Names are clipped by rendered width
with an ellipsis, not by chopping UTF-8 bytes. The font adds approximately 1 MB
of constant data to the launcher core; its complete license notice is retained
in the executable. Font sources and redistribution notices remain in
`third_party/pkg_font/`. Desktop layout and terminal rendering are unchanged.

- Arrows select actions/networks; left/right switch between the two top actions.
- After a fresh scan, if the page is still open and no connection/stop request
  is active, connect once to the strongest saved supported network. Saved rows
  precede unknown networks, so a remembered network is not dropped merely
  because eight stronger unknown hotspots are nearby. A failed attempt reports
  its error and does not start an automatic retry loop.
- OK selects a network. An unambiguous current network does not reprompt.
  A saved network goes directly to the connection page using its existing
  supplicant profile, with no password entry. Rows show `SAVED` explicitly.
  Unremembered secured networks still open the visible-password input page.
- Same-name networks with different security are treated as ambiguous, rather
  than claiming both are the current connection.
- Password entry is visible by default. Tab hides/reveals it; Shift changes the
  keyboard layer. OK or keyboard Enter submits in alphabet mode; OK inserts the
  selected symbol in symbol mode, while keyboard Enter still submits.
- Passwords are validated before starting work (8–63 printable ASCII characters
  for WPA/WPA2-PSK). Invalid/busy submissions preserve the typed password.
- A failed connection returns to visible editing only if the user is still on
  the Wi-Fi page. Back, Home, locking, and successful connection clear it.
- Back during service work leaves the page while work continues. Selecting Off
  requests cooperative cancellation and waits for restoration before disabling.

## Service behavior

The parent UI owns the busy state. A forked worker reports phase changes through
atomic pipe records and receives cancellation through a separate pipe. Repeated
phase callbacks generate no traffic. The parent distinguishes progress records,
final snapshots, and failed/partial results. UI shutdown allows cooperative
Wi-Fi restoration before its fallback process termination.

The service validates authentication against both the target SSID and newly
created network ID before acquiring an address, and rechecks authentication
before saving. Existing network entries and configuration bytes are preserved;
failures/cancellation attempt to restore them. Configuration is saved only after
verified authentication and address acquisition.

Scanning waits for scan completion and traverses BSS records rather than using
a possibly stale/truncated `SCAN_RESULTS` table. Saved supported network /
security combinations take priority; each group is sorted by signal, with eight
rows retained overall. Unsupported security is shown explicitly. Saved profile
matching checks both decoded SSID and authentication type, and verifies that a
PSK exists without sending it to the UI. Reconnection selects the existing
profile: it does not add/remove it, rewrite its password, or save configuration.
Failure/cancellation restores the previous selection and enabled state.

Hardware initialization loads only the fixed device module when necessary,
uses a bounded wait for wlan0, and clears only its identifiable radio's software
block. Vendor scripts with global process termination are not used. DHCP process
termination is restricted to the project's interface and PID-file arguments.

After a launcher restart, passive status observation can discover an existing
connection without enabling the interface or changing credentials. Authentication
completion and an interface address are both required. Observation has a short
total control-socket budget and caches both successful and failed probes; an
expired positive result is cleared if the next query fails. Explicitly disabling
Wi-Fi and in-progress/error states take precedence over passive discovery.

## Local verification and previews

From the project root in WSL:

    make -j4 BUILD_DIR=build/wifi-redesign \
      build/wifi-redesign/host-tests \
      build/wifi-redesign/host-ui-lifecycle-tests wifi-service-test wifi-preview
    build/wifi-redesign/host-tests
    build/wifi-redesign/host-ui-lifecycle-tests
    make -j4 BUILD_DIR=build/wifi-redesign all verify

On a host with Pillow installed:

    python scripts/render-wifi-preview.py build/wifi-redesign

The preview executable uses the production renderer, not a separately drawn
mockup. It produces eight states, including Chinese/long names, a second page,
connection progress, safe stop, disabled Wi-Fi, and password validation. The
combined preview is `build/wifi-redesign/wifi-preview-sheet.png`.

Host UI tests cover key routing, busy/invalid submission, password masking,
UTF-8 glyphs, clipping, page boundaries, and ambiguous same-name security.
Lifecycle tests cover progress/final IPC records, cancellation, cooperative
shutdown, failed snapshot handling, and avoiding late navigation on completion.
Service simulations replace private I/O and never access real network devices,
credentials, modules, or sysfs.

## Hardware acceptance still required

The extracted target root filesystem exposes an important ownership boundary: `/etc/init.d/S40network` starts `ifup -a`, and `bin/wifi_up.sh` starts `wpa_supplicant` with `ctrl_interface=/var/run/wpa_supplicant` from `/usr/resource/wpa_supplicant.conf`, then starts a background `udhcpc`. `bin/wifi_down.sh` later uses broad `killall` and terminates the factory daemon. The service therefore adopts a live factory `wlan0` control socket and its configuration instead of starting a second daemon; it never invokes the broad vendor stop script. DHCP ownership remains restricted to the exact `wlan0` plus `/run/c1/udhcpc.pid` process.

A scan can receive `FAIL-BUSY` while the factory daemon is completing its startup scan. The service now waits for `STATUS` to leave `INTERFACE_DISABLED` after re-enabling the interface, retries only `FAIL-BUSY` within the operation deadline, and never treats a stale event as the new scan result. A small `/run/c1/wifi.disabled` marker preserves an intentional Off state across a launcher restart; a reachable control socket alone no longer means Wi-Fi is enabled.

These lifecycle paths have host regressions and a private Unix-datagram control-daemon test (`make wifi-control-lifecycle-test`). They still do not prove the atbm603x driver, RF kill state, DHCP hook, route, DNS, or an actual access point on hardware. The USB target was offline during this review, so no device state was changed.

- Validate cold start, the firmware's actual supplicant control capabilities,
  rfkill layout, BusyBox DHCP hooks, and existing factory-owned services.
- Exercise open and WPA/WPA2-PSK networks, wrong passwords, DHCP failure, repeated
  key presses, cancellation, sleep/resume, and restart while already connected.
- Check physical readability, partial refresh ghosting, and refresh latency.

Pure WPA3-SAE, enterprise authentication, WEP, and OWE remain unsupported.
Foreign supplicant/DHCP ownership is reported rather than forcibly taken over.
Scan requires ATTACH, BSS_FLUSH, and BSS FIRST/NEXT. Older single-page network
lists are accepted only with the standard 4096-byte reply size and sufficient
space to establish completeness; near-limit lists still require pagination.
Turning Wi-Fi off lowers the interface and preserves the managed supplicant;
it does not promise module unloading or complete radio power removal. Forced
process death or an unresponsive daemon can still prevent complete restoration.
