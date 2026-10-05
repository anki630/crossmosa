"""
PlatformIO post script (v364, reproducible builds): strip firmware.elf before
firmware.bin is generated from it, and keep the full ELF as firmware.debug.elf.

Why: esptool's `elf2image` writes the SHA-256 of the whole ELF file into the app
description (offset 0xB0), and the image digest at the end covers that field. The
ELF's debug info and symbol table carry the absolute paths the build ran in (the
project directory, ~/.platformio). So the same source built in another directory,
or on another machine, produced a firmware.bin that differed in those 32 + 33 bytes
even though everything the device loads was identical.

With debug info and symbols removed, the ELF holds only the loaded sections and the
section headers. Verified 2026-10-05 on 2.2.0-beta.2: two clones in differently named
directories built byte-identical firmware.bin files, and an image made from the
stripped ELF differs from one made from the full ELF only in the ELF hash (0xB0, 32
bytes) and the trailing digest (33 bytes).

Keep firmware.debug.elf for addr2line / crash forensics: the stripped ELF has no
symbols.
"""

import shutil
import subprocess

Import("env")  # noqa: F821  -- provided by PlatformIO at script load


def _objcopy(build_env):
    # The C compiler is e.g. riscv32-esp-elf-gcc (C3) or xtensa-esp32s3-elf-gcc (S3);
    # objcopy from the same toolchain sits next to it on the build PATH.
    cc = build_env.subst("$CC")
    if not cc.endswith("gcc"):
        raise RuntimeError(f"strip_elf_for_image: unexpected compiler name {cc!r}")
    return cc[: -len("gcc")] + "objcopy"


def strip_for_image(target, source, env):  # noqa: ARG001 -- SCons action signature
    elf = target[0].get_abspath()
    debug_elf = elf[: -len(".elf")] + ".debug.elf"
    shutil.copyfile(elf, debug_elf)
    subprocess.check_call([_objcopy(env), "--strip-all", elf], env=env["ENV"])
    print(f"strip_elf_for_image: full ELF kept as {debug_elf}; {elf} stripped for the image")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", strip_for_image)  # noqa: F821
