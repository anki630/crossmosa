"""
PlatformIO pre-build script: move PNGdec's two big inline buffers out of the PNG object.

Why (CrossMosa v245, 2026-09-14): `sizeof(PNG)` on the device is 59,456 bytes with
PNG_MAX_BUFFERED_PIXELS=16416 -- a single allocation larger than the ~53KB contiguous
ceiling the internal heap settles at after a few chapters (CLAUDE.md hard limit 6).
Once p2 is split, every PNG illustration fails with `png-alloc-decoder` until reboot
(diag244-2.log; the same image already failed on v191). With the 32K zlib window and
the row buffer caller-provided, the largest piece is ~40KB and the object ~3KB.

Changes (all in the PNGdec libdep, both envs):
  PNGdec.h  ucZLIB[...] / ucPixels[...] arrays  -> pointers; PNG_ZLIB_BUF_SIZE macro;
            PNG::setBuffers() (call after open(), which memsets the struct, before decode()).
  png.inl   sizeof(pPage->ucPixels) -> PNG_MAX_BUFFERED_PIXELS (would be sizeof(pointer));
            DecodePNG refuses to run with unset buffers and returns PNG_NO_BUFFER (upstream's early
            error returns are `return 0`, which callers read as PNG_SUCCESS -- not copied here).

Why not `git apply` like patch_jpegdec.py: PNGdec comes from the registry (no .git of its
own) and lives inside this repository's worktree -- `git apply` run there treats patch
paths as relative to the enclosing repo root and silently skips them.

Idempotent exact-text replacement: new text present -> skip; old text present exactly
once -> replace; otherwise abort the build (the libdep drifted; never half-patch).
"""

Import("env")  # noqa: F821 (SCons-injected global)
import json
import os
import sys

EXPECTED_VERSION = "1.1.6"

REPLACEMENTS = [
    (
        "PNGdec.h",
        "    uint8_t ucZLIB[32768 + sizeof(struct inflate_state)]; // put this here to avoid needing malloc/free\n",
        "    uint8_t *ucZLIB; // CrossMosa: caller-provided, PNG_ZLIB_BUF_SIZE bytes (see PNG::setBuffers)\n",
    ),
    (
        "PNGdec.h",
        "    uint8_t ucPixels[PNG_MAX_BUFFERED_PIXELS];\n",
        "    uint8_t *ucPixels; // CrossMosa: caller-provided, PNG_MAX_BUFFERED_PIXELS bytes (see PNG::setBuffers)\n",
    ),
    (
        "PNGdec.h",
        "} PNGIMAGE;\n",
        "} PNGIMAGE;\n"
        "// CrossMosa: size of the caller-provided zlib state + 32K window buffer.\n"
        "#define PNG_ZLIB_BUF_SIZE (32768 + sizeof(struct inflate_state))\n",
    ),
    (
        "PNGdec.h",
        "class PNG\n{\n  public:\n",
        "class PNG\n{\n  public:\n"
        "    // CrossMosa: the decoder's two big buffers live outside the object so it never needs one\n"
        "    // ~59KB contiguous block. open() zeroes the struct, so call this after open(), before decode().\n"
        "    void setBuffers(uint8_t *pZlib, uint8_t *pPixels) { _png.ucZLIB = pZlib; _png.ucPixels = pPixels; }\n",
    ),
    (
        "png.inl",
        "                    d = (uint16_t *)&pPage->ucPixels[sizeof(pPage->ucPixels)-512];\n",
        "                    d = (uint16_t *)&pPage->ucPixels[PNG_MAX_BUFFERED_PIXELS-512]; // CrossMosa: was sizeof(array)\n",
    ),
    (
        "png.inl",
        "                                pngd.pFastPalette = (iOptions & PNG_FAST_PALETTE) ? (uint16_t *)&pPage->ucPixels[sizeof(pPage->ucPixels)-512] : NULL;\n",
        "                                pngd.pFastPalette = (iOptions & PNG_FAST_PALETTE) ? (uint16_t *)&pPage->ucPixels[PNG_MAX_BUFFERED_PIXELS-512] : NULL; // CrossMosa: was sizeof(array)\n",
    ),
    (
        "png.inl",
        "        pPage->iError = PNG_NO_BUFFER;\n        return 0;\n    }\n    // Use internal buffer to maintain the current and previous lines\n",
        "        pPage->iError = PNG_NO_BUFFER;\n        return 0;\n    }\n"
        "    if (pPage->ucZLIB == NULL || pPage->ucPixels == NULL) { // CrossMosa: setBuffers() not called\n"
        "        pPage->iError = PNG_NO_BUFFER;\n        return PNG_NO_BUFFER; // not 0: 0 == PNG_SUCCESS to callers\n    }\n"
        "    // Use internal buffer to maintain the current and previous lines\n",
    ),
]


def patch_pngdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in sorted(os.listdir(libdeps_dir)):
        lib_dir = os.path.join(libdeps_dir, env_dir, "PNGdec")
        if not os.path.isdir(os.path.join(lib_dir, "src")):
            continue
        _check_version(lib_dir)
        for name, old, new in REPLACEMENTS:
            _replace_once(os.path.join(lib_dir, "src", name), old, new)


def _check_version(lib_dir):
    meta = os.path.join(lib_dir, ".piopm")
    version = None
    try:
        with open(meta, "r", encoding="utf-8") as f:
            version = json.load(f).get("version")
    except (OSError, ValueError):
        pass
    if version != EXPECTED_VERSION:
        sys.stderr.write(
            "ERROR: PNGdec patch expects version %s but %s reports %r -- re-verify the patch.\n"
            % (EXPECTED_VERSION, lib_dir, version)
        )
        raise SystemExit(1)


def _replace_once(path, old, new):
    with open(path, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    if new in text:
        return
    count = text.count(old)
    if count != 1:
        sys.stderr.write(
            "ERROR: PNGdec patch anchor found %d times in %s (expected 1):\n%s\n" % (count, path, old)
        )
        raise SystemExit(1)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text.replace(old, new, 1))
    print("Patched PNGdec: %s" % os.path.relpath(path))


patch_pngdec(env)  # noqa: F821
