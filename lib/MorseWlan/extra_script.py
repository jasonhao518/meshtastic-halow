"""
PlatformIO doesn't natively link .o files vendored inside a library directory.
The Morse Micro SDK ships the chip firmware and board configuration file as
pre-built .mbin.o objects containing data sections. This script appends the
selected objects to LINKFLAGS so they land in the final ELF.
"""

import os

Import("env", "projenv")

LIB_DIR = os.path.join(env.subst("$PROJECT_DIR"), "lib", "MorseWlan")


def _get_define(name, default):
    for define in env.get("CPPDEFINES", []):
        if isinstance(define, tuple) and define[0] == name:
            return str(define[1]).replace('\\"', '"').strip("\"'")
        if isinstance(define, str):
            prefix = name + "="
            if define.startswith(prefix):
                return define[len(prefix) :].replace('\\"', '"').strip("\"'")
    return default


fw_file = _get_define("CONFIG_MM_FW_FILE", "mm6108.mbin")
bcf_file = _get_define("CONFIG_MM_BCF_FILE", "bcf_mf08651_us.mbin")

mbin_objects = [
    os.path.join(LIB_DIR, "src", fw_file + ".o"),
    os.path.join(LIB_DIR, "src", bcf_file + ".o"),
]

# Only add objects that actually exist; missing ones surface as link errors,
# not silent corruption.
for obj in mbin_objects:
    if not os.path.isfile(obj):
        print("warning: mm-iot-esp32 blob missing: %s" % obj)

env.Append(LINKFLAGS=mbin_objects)
