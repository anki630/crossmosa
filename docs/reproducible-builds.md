# Reproducible builds

A release build of this firmware is **bit-for-bit reproducible**, so anyone can check that
a published `firmware.bin` really was built from the published source.

## How

```bash
export SOURCE_DATE_EPOCH=1786060800     # the release commit's timestamp, UTC seconds
pio run -e gh_release
sha256sum .pio/build/gh_release/firmware.bin
```

Building twice from a clean tree with the same `SOURCE_DATE_EPOCH` produces an identical
`firmware.bin`. The value itself is arbitrary but must match the one used for the release
you are checking; each release publishes its own (see the release notes).

`SOURCE_DATE_EPOCH` is the
[cross-project convention](https://reproducible-builds.org/docs/source-date-epoch/) for
this. Two things in this build honour it:

1. **GCC** substitutes it for `__DATE__` / `__TIME__` (verified with the toolchain this
   project pins, `riscv32-esp-elf-g++ 14.2.0`). Two translation units bake those in —
   `src/activities/settings/SdFirmwareUpdateActivity.cpp` and, in the Arduino core,
   `cores/esp32/chip-debug-report.cpp`. The second is not ours to patch, which is why the
   environment variable is the mechanism rather than a source change.
2. **`scripts/build_html.py`** stamps it into the gzip headers of the compressed web-UI
   assets. Without it the gzip MTIME field is the wall clock, and five assets each carry a
   different four-byte timestamp into the image.

## What is *not* a source of variation

Two further regions of the image differ whenever anything else does, but they are
consequences rather than causes, and they converge once the two inputs above are fixed:

* offset `0xb0`, 32 bytes — `app_elf_sha256` in `esp_app_desc_t`, i.e. the hash of the ELF.
* the last 33 bytes — the image SHA-256 and checksum byte appended by `esptool`.

If a diff shows *only* those two regions, something upstream of them still varies; the
diff is telling you the ELF changed, not why.

## Verifying a published binary

```bash
git checkout <release tag>
export SOURCE_DATE_EPOCH=<value from the release notes>
pio run -e gh_release
cmp .pio/build/gh_release/firmware.bin /path/to/downloaded/firmware.bin
```

A mismatch is worth reporting. Note that the toolchain version matters: a different
`riscv32-esp-elf-g++` or a different pinned platform release will produce a different
(equally valid) binary.
