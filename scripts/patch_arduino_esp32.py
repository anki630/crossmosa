"""PlatformIO pre-build script: stop arduino-esp32 compiling RainMaker/Insights.

Only needed because platformio.ini sets `custom_sdkconfig`, which triggers a full
ESP-IDF framework rebuild (the only way to reach the CONFIG_BT_CTRL_* / NimBLE
pool sizes — plain -D flags are overridden by the precompiled sdkconfig.h). That
rebuild compiles arduino-esp32's whole library set, including RainMaker and
Insights, which #include esp_rmaker_core.h from the `espressif/esp_rainmaker`
component that `custom_component_remove` strips. Result without this patch:

    *** [.../https_server.crt.S.o] Source `.../https_server.crt.S' not found

The documented upstream escape hatch (CONFIG_ARDUINO_SELECTIVE_RainMaker=n) does
NOT work on the pioarduino fork this project pins: those Kconfig symbols are not
declared there, so the guard in CMakeLists evaluates true and the library builds
anyway. Patching the library list directly is what actually works.

Approach and diagnosis both come from imshentastic/CrumBLE
(scripts/patch_arduino_esp32.py), which burned five build attempts arriving at it.

Idempotent: a marker line prevents re-patching an already-patched framework.
"""

import os

Import("env")  # noqa: F821 — injected by PlatformIO

MARKER = "# crosspoint-tc: RainMaker/Insights stripped (esp_rainmaker component removed)"
STRIP_LIBRARIES = ["RainMaker", "Insights"]


def _strip_libraries(cmake_path):
    with open(cmake_path, "r") as f:
        content = f.read()

    if MARKER in content:
        return

    stripped = 0
    for lib in STRIP_LIBRARIES:
        # These sit one per line inside the ARDUINO_ALL_LIBRARIES set() block.
        # Comment out rather than delete so the change stays greppable.
        entry = "  %s\n" % lib
        if entry in content:
            content = content.replace(entry, "  # %s (stripped by patch_arduino_esp32.py)\n" % lib, 1)
            stripped += 1

    if stripped == 0:
        print(
            "WARNING: patch_arduino_esp32: none of %s found in %s — the framework "
            "may have changed; the build will fail on esp_rmaker_core.h if so"
            % (",".join(STRIP_LIBRARIES), cmake_path)
        )
        return

    with open(cmake_path, "w") as f:
        f.write(MARKER + "\n" + content)
    print("patch_arduino_esp32: stripped %d libraries (%s)" % (stripped, ",".join(STRIP_LIBRARIES)))


def main():
    package = env.PioPlatform().get_package("framework-arduinoespressif32")  # noqa: F821
    if not package:
        print("patch_arduino_esp32: framework not installed yet, skipping")
        return
    cmake_path = os.path.join(package.path, "CMakeLists.txt")
    if not os.path.isfile(cmake_path):
        print("patch_arduino_esp32: CMakeLists.txt not found at %s" % cmake_path)
        return
    _strip_libraries(cmake_path)


main()
