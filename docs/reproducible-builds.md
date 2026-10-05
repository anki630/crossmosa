# Reproducible builds

A release build of this firmware is **bit-for-bit reproducible**, so anyone can check that
a published `update.bin` really was built from the published source.

## How

```bash
export SOURCE_DATE_EPOCH=1786060800     # the release commit's timestamp, UTC seconds
pio run -e gh_release
sha256sum .pio/build/gh_release/firmware.bin
```

Building from a clean tree with the same `SOURCE_DATE_EPOCH` produces an identical
`firmware.bin` (published as `update.bin`). The value itself is arbitrary but must match
the one used for the release you are checking; each release publishes its own (see the
release notes). `scripts/mk-release.sh` sets it to the release commit's own timestamp.

It does not matter where the tree is checked out. For 2.2.0-beta.2, two clones in
differently named directories produced byte-identical `update.bin`, firmware zip and
manual EPUB. A build on another machine is expected to match as long as it uses the same
pinned platform (and therefore the same toolchain); that has not been tested on a second
machine yet.

## What would otherwise vary, and how each is pinned

1. **`__DATE__` / `__TIME__`.** GCC substitutes `SOURCE_DATE_EPOCH` for them (verified
   with the toolchain this project pins, `riscv32-esp-elf-g++ 14.2.0`). Two translation
   units bake those in — `src/activities/settings/SdFirmwareUpdateActivity.cpp` and, in the
   Arduino core, `cores/esp32/chip-debug-report.cpp`. The second is not ours to patch,
   which is why the environment variable is the mechanism rather than a source change.
2. **Compressed web-UI assets.** `scripts/build_html.py` stamps `SOURCE_DATE_EPOCH` into
   their gzip headers. Without it the gzip MTIME field is the wall clock, and five assets
   each carry a different four-byte timestamp into the image.
3. **`__FILE__` paths.** Log macros in the Arduino core expand `__FILE__`, which would put
   the absolute path of `~/.platformio` into the image. `platformio.ini` maps the packages
   directory to `/pio` with `-fmacro-prefix-map`.
4. **The ESP-IDF app description.** By default it records the git describe of the tree,
   the project folder name and the build time. `custom_sdkconfig` turns all three off
   (`CONFIG_APP_EXCLUDE_PROJECT_VER_VAR`, `CONFIG_APP_EXCLUDE_PROJECT_NAME_VAR`,
   `CONFIG_APP_COMPILE_TIME_DATE=n`). The firmware reports its version from
   `CROSSPOINT_VERSION`, not from this description.
5. **The ELF hash in the image.** `esptool elf2image` writes the SHA-256 of the whole ELF
   file into the app description (offset `0xb0`, 32 bytes), and the image SHA-256 and
   checksum appended at the end (33 bytes) cover it. The ELF's debug info and symbol
   table hold the absolute paths the build ran in, so the same source built in another
   directory differed in exactly those 65 bytes. `scripts/strip_elf_for_image.py` strips
   debug info and symbols from `firmware.elf` before the image is made; the full ELF is
   kept as `firmware.debug.elf`. An image made from the stripped ELF differs from one made
   from the full ELF only in those 65 bytes; everything the device loads is the same.

If two builds differ *only* at `0xb0` and in the last 33 bytes, the code is the same and
something still differs in the ELF that is hashed. Compare the two `firmware.elf` files
to find out what.

## Verifying a published binary

```bash
git checkout <release tag>
git submodule update --init --recursive --depth 1
export SOURCE_DATE_EPOCH=<value from the release notes>
pio run -e gh_release
cmp .pio/build/gh_release/firmware.bin /path/to/downloaded/update.bin
```

A mismatch is worth reporting. Note that the toolchain version matters: a different
`riscv32-esp-elf-g++` or a different pinned platform release will produce a different
(equally valid) binary.

## Crash forensics

`firmware.elf` no longer has symbols. To turn a crash address into a function and line,
use the full ELF:

```bash
riscv32-esp-elf-addr2line -f -C -e .pio/build/gh_release/firmware.debug.elf <address>
```
