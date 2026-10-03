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

v358 (2026-09-30): the zlib buffer is further split in two -- the ~7KB inflate state and the 32K window --
because PNGdec's inflate never allocates or frees the window itself (zcalloc/zcfree are no-ops); it only
follows state->window. diag-prev357 (X3): `png-alloc-zlib 39896 tr=1 max=38900 free=87064` -- enough memory in
total, but p2 was cut into 32.7KB + 39.1KB by long-lived small blocks and no piece reached 39,896.
  PNGdec.h  ucZWindow pointer (NULL = window follows the state inside ucZLIB, the v245 layout);
            PNG_ZLIB_STATE_SIZE / PNG_ZLIB_WINDOW_SIZE; 3-argument PNG::setBuffers().
  png.inl   state->window = ucZWindow when set.
  The v358 entries only APPEND after v245's new text (or touch lines v245 never touched), so v245's
  "new text present -> skip" check keeps holding on trees patched by either version.

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
    # v358: separate 32K window (see the docstring). Must stay after the v245 entries they extend.
    (
        "PNGdec.h",
        "    uint8_t *ucZLIB; // CrossMosa: caller-provided, PNG_ZLIB_BUF_SIZE bytes (see PNG::setBuffers)\n",
        "    uint8_t *ucZLIB; // CrossMosa: caller-provided, PNG_ZLIB_BUF_SIZE bytes (see PNG::setBuffers)\n"
        "    uint8_t *ucZWindow; // CrossMosa v358: separate 32K window; NULL = it follows the state inside ucZLIB\n",
    ),
    (
        "PNGdec.h",
        "#define PNG_ZLIB_BUF_SIZE (32768 + sizeof(struct inflate_state))\n",
        "#define PNG_ZLIB_BUF_SIZE (32768 + sizeof(struct inflate_state))\n"
        "// CrossMosa v358: the same memory as two separate blocks (see the 3-argument PNG::setBuffers).\n"
        "#define PNG_ZLIB_STATE_SIZE (sizeof(struct inflate_state))\n"
        "#define PNG_ZLIB_WINDOW_SIZE 32768\n",
    ),
    (
        "PNGdec.h",
        "    void setBuffers(uint8_t *pZlib, uint8_t *pPixels) { _png.ucZLIB = pZlib; _png.ucPixels = pPixels; }\n",
        "    void setBuffers(uint8_t *pZlib, uint8_t *pPixels) { _png.ucZLIB = pZlib; _png.ucPixels = pPixels; }\n"
        "    // CrossMosa v358: zlib state (PNG_ZLIB_STATE_SIZE) and window (PNG_ZLIB_WINDOW_SIZE) as two blocks, so the\n"
        "    // decoder never needs one ~40KB contiguous block. Same rule: after open(), before decode().\n"
        "    void setBuffers(uint8_t *pZlibState, uint8_t *pZlibWindow, uint8_t *pPixels) {\n"
        "        _png.ucZLIB = pZlibState; _png.ucZWindow = pZlibWindow; _png.ucPixels = pPixels;\n"
        "    }\n",
    ),
    (
        "png.inl",
        "    state->window = &pPage->ucZLIB[sizeof(struct inflate_state)]; // point to 32k dictionary buffer\n",
        "    state->window = pPage->ucZWindow ? pPage->ucZWindow : &pPage->ucZLIB[sizeof(struct inflate_state)]; // CrossMosa v358: separate 32K window when set\n",
    ),
]


def patch_pngdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    # v358: every anchor in every env is checked in memory first; files are written only after all of them
    # succeeded (codex: a half-patched tree -- new 3-argument API, old window line -- would let inflate write a
    # 32K window past the ~7KB state block; and one env must not be written when another env's anchor fails).
    texts = {}  # path -> [original, patched]
    for env_dir in sorted(os.listdir(libdeps_dir)):
        lib_dir = os.path.join(libdeps_dir, env_dir, "PNGdec")
        if not os.path.isdir(os.path.join(lib_dir, "src")):
            continue
        _check_version(lib_dir)
        for name, old, new in REPLACEMENTS:
            path = os.path.join(lib_dir, "src", name)
            if path not in texts:
                with open(path, "r", encoding="utf-8", newline="") as f:
                    original = f.read()
                texts[path] = [original, original]
            texts[path][1] = _replace_once(path, texts[path][1], old, new)
    for path, (original, patched) in texts.items():
        if patched == original:
            continue
        tmp = path + ".crossmosa-tmp"
        with open(tmp, "w", encoding="utf-8", newline="") as f:
            f.write(patched)
        os.replace(tmp, path)
        print("Patched PNGdec: %s" % os.path.relpath(path))


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


def _replace_once(path, text, old, new):
    if new in text:
        return text
    count = text.count(old)
    if count != 1:
        sys.stderr.write(
            "ERROR: PNGdec patch anchor found %d times in %s (expected 1):\n%s\n" % (count, path, old)
        )
        raise SystemExit(1)
    return text.replace(old, new, 1)


patch_pngdec(env)  # noqa: F821
