# Third-party notices

## libtsm

This project vendors `kmscon/libtsm` version 4.7.1, commit
`ef0a1a40c30d164913f413c47de5bbd8383a6daa`, from:

https://github.com/kmscon/libtsm

The libtsm terminal state machine is distributed under the MIT license. The
vendored source and its full terms are in `third_party/libtsm/COPYING`.

libtsm includes a hash-table implementation derived from CCAN and distributed
under LGPL-2.1-or-later. Its terms are in
`third_party/libtsm/LICENSE_htable`, and the corresponding source is retained in
`third_party/libtsm/src/shared/shl-htable.c` and `.h`.

The vendored Unicode width implementation is distributed under the MIT license.
Its terms are in `third_party/libtsm/external/wcwidth/LICENSE.txt`.

The complete source, target flags, and object-level build rules needed to
modify and relink these components are included in this repository's
`Makefile`. The installed `C1ancher` binary statically contains the listed
components; no separate libtsm runtime is required on the device.

## ed25519

The `c1pkg` package manager vendors the portable Ed25519 implementation by
Orson Peters (`orlp/ed25519`), based on SUPERCOP ref10, from:

https://github.com/orlp/ed25519

It is distributed under the permissive zlib license. The full terms are in
`third_party/ed25519/LICENSE.txt`. Only the verification, SHA-512, scalar and
field arithmetic sources are linked into the static `c1pkg` executable; key
generation and signing remain administrator-side operations.

## Neofetch

This project vendors Neofetch version 7.1.0 by Dylan Araps from:

https://github.com/dylanaraps/neofetch

Neofetch is distributed under the MIT license. The unmodified upstream script
is retained in `third_party/neofetch/neofetch.upstream`, and its full license
is retained in `third_party/neofetch/LICENSE.md`. The adjacent `neofetch`
launcher and `c1-config.conf` are C1-Slim-specific integration files. The
project installer deploys these files to `/usr/data/c1/` and does not embed
Neofetch into the statically linked C1ancher executable.