"""Pre-build guard: refuse to build if any tracked text source contains a NUL byte.

Why this exists (2026-08-24): a scripted edit wrote a literal 0x00 into
lib/GfxRenderer/GfxRenderer.cpp (an intended `'\\0'` became a real NUL). The code
compiled and ran correctly -- the damage was to the toolchain: grep/rg classify
the whole file as binary and silently report ZERO matches with exit status 0 or 1
and no warning. A 46-item removal audit and a dead-entry scan both ran while
blind to an 86 KB core rendering file, and neither noticed.

git grep -I (used by scripts/preflight-public.sh) only sniffs the first 8000
bytes for its binary check, so a NUL early in a file would silently blind the
public-repo secret scan too. That is the real reason this guard is cheap
insurance rather than pedantry.
"""

import subprocess
import sys

TEXT_SUFFIXES = (
    ".c", ".cc", ".cpp", ".h", ".hpp", ".ini", ".py", ".sh", ".md",
    ".yaml", ".yml", ".json", ".html", ".js", ".css", ".csv", ".txt",
)


def main() -> int:
    try:
        tracked = subprocess.run(
            ["git", "ls-files", "-z"], capture_output=True, check=True
        ).stdout.split(b"\0")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return 0  # not a git checkout -- nothing to guard

    bad = []
    for raw in tracked:
        if not raw:
            continue
        path = raw.decode("utf-8", "replace")
        if not path.endswith(TEXT_SUFFIXES):
            continue
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except OSError:
            continue
        if b"\0" in data:
            bad.append((path, data.index(b"\0")))

    if bad:
        print("\n*** NUL byte in text source -- grep/rg will treat these files as")
        print("*** binary and silently report zero matches. Fix before building.")
        for path, off in bad:
            print(f"      {path}  (first NUL at byte {off})")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
