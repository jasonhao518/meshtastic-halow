"""
Build the Morse Micro mm-iot-esp32 SDK wrapper from the git submodule on demand.

PlatformIO is not consuming the SDK's ESP-IDF CMake components here, so this
script compiles the ESP32 shim/support sources, merges them with the SDK's
Morselib archive into a build-local archive, and converts the selected firmware
and BCF blobs into linkable objects.
"""

import os
import re
import subprocess

Import("env")

PROJECT_DIR = env.subst("$PROJECT_DIR")
BUILD_DIR = env.subst("$BUILD_DIR")
SDK_DIR = os.path.join(PROJECT_DIR, "third_party", "mm-iot-esp32", "framework")
SDK_MORSELIB = os.path.join(SDK_DIR, "morselib")
SDK_SHIMS = os.path.join(SDK_DIR, "mm_shims")
SDK_SRC = os.path.join(SDK_DIR, "src")
SDK_FIRMWARE = os.path.join(SDK_DIR, "morsefirmware")
OUT_DIR = os.path.join(BUILD_DIR, "mm_iot_esp32")


def _get_define(name, default):
    for define in env.get("CPPDEFINES", []):
        if isinstance(define, tuple) and define[0] == name:
            return str(define[1]).replace('\\"', '"').strip("\"'")
        if isinstance(define, str):
            prefix = name + "="
            if define.startswith(prefix):
                return define[len(prefix):].replace('\\"', '"').strip("\"'")
    return default


def _find_blob(name):
    basename = os.path.basename(name)
    roots = [
        os.path.join(PROJECT_DIR, "morsefirmware"),
        SDK_FIRMWARE,
        os.path.join(PROJECT_DIR, "lib", "MorseWlan", "src"),
    ]
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, _, filenames in os.walk(root):
            if basename in filenames:
                return os.path.join(dirpath, basename)
            object_name = basename + ".o"
            if object_name in filenames:
                return os.path.join(dirpath, object_name)
    return os.path.join(SDK_FIRMWARE, basename)


def _binary_symbol(path, suffix):
    return "_binary_" + re.sub(r"[^A-Za-z0-9_]", "_", path) + "_" + suffix


def _run(cmd, **kwargs):
    print(" ".join(cmd))
    subprocess.check_call(cmd, **kwargs)


def _toolchain_program(suffix):
    ar = env.subst("$AR")
    prefix = re.sub(r"(gcc-)?ar$", "", ar)
    return prefix + suffix


def _build_mbin_object(target, source, env):
    source_path = str(source[0])
    target_path = str(target[0])
    os.makedirs(os.path.dirname(target_path), exist_ok=True)
    if source_path.endswith(".o"):
        _run(["cp", source_path, target_path])
        return

    prefix = env["MM_PREFIX"]
    objcopy = _toolchain_program("objcopy")
    _run([
        objcopy,
        "-I",
        "binary",
        "-O",
        "elf32-xtensa-le",
        "-B",
        "xtensa",
        source_path,
        target_path,
        "--redefine-sym",
        f"{_binary_symbol(source_path, 'start')}={prefix}_start",
        "--redefine-sym",
        f"{_binary_symbol(source_path, 'end')}={prefix}_end",
        "--rename-section",
        ".data=.rodata._fw_mbin,contents,alloc,load,readonly,data",
        "--set-section-alignment",
        ".data=4",
    ])


def _build_archive(target, source, env):
    target_path = str(target[0])
    libmorse = str(source[0])
    objects = [str(s) for s in source[1:]]
    ar = env.subst("$AR")
    ranlib = env.subst("$RANLIB")
    os.makedirs(os.path.dirname(target_path), exist_ok=True)
    script = ["CREATE " + target_path, "ADDLIB " + libmorse]
    script.extend("ADDMOD " + obj for obj in objects)
    script.extend(["SAVE", "END"])
    print(f"Creating {target_path}")
    proc = subprocess.Popen([ar, "-M"], stdin=subprocess.PIPE)
    proc.communicate(("\n".join(script) + "\n").encode())
    if proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, [ar, "-M"])
    if ranlib:
        _run([ranlib, target_path])


if not os.path.isdir(SDK_DIR):
    raise RuntimeError("third_party/mm-iot-esp32 submodule is missing; run git submodule update --init --recursive")

target = "esp32s3"
libmorse = os.path.join(SDK_MORSELIB, "lib", target, "libmorse.a")
if not os.path.isfile(libmorse):
    raise RuntimeError(f"missing mm-iot-esp32 Morselib archive: {libmorse}")

include_dirs = [
    os.path.join(SDK_MORSELIB, "include"),
    os.path.join(SDK_SHIMS, "include", target),
    os.path.join(SDK_SRC, "mmutils"),
    os.path.join(SDK_SRC, "mmpktmem"),
    os.path.join(SDK_SRC, "mmregdb"),
    os.path.join(SDK_SRC, "mmipal"),
    os.path.join(SDK_SRC, "mmipal", "lwip"),
]

shim_sources = [
    os.path.join(SDK_SHIMS, "mmosal_shim_freertos_esp32.c"),
    os.path.join(SDK_SHIMS, "mmhal_core.c"),
    os.path.join(SDK_SHIMS, "mmhal_os.c"),
    os.path.join(SDK_SHIMS, "mmhal_wlan.c"),
    os.path.join(SDK_SHIMS, "mmhal_wlan_binaries.c"),
    os.path.join(SDK_SRC, "mmpktmem", "mmpktmem_heap.c"),
    os.path.join(SDK_SRC, "mmutils", "mmbuf.c"),
    os.path.join(SDK_SRC, "mmutils", "mmcrc.c"),
    os.path.join(SDK_SRC, "mmutils", "mmutils_wlan.c"),
    os.path.join(SDK_SRC, "mmregdb", "mmregdb.c"),
]

build_env = env.Clone()
build_env.Prepend(CPPPATH=include_dirs)
build_env.Append(
    CPPDEFINES=[
        "CONFIG_IEEE80211AH",
        "MM_IOT",
        ("CONFIG_MMHAL_CHIP_TYPE_MM6108", 1),
        ("CONFIG_MMHAL_CHIP_TYPE_MM8108", 0),
        ("CONFIG_MM_EXPERIMENTAL_MESH", 1),
        ("CONFIG_MM_EXPERIMENTAL_MESH_OP_CLASS", 1),
        ("CONFIG_MM_EXPERIMENTAL_MESH_CHAN", 27),
        ("CONFIG_MM_EXPERIMENTAL_MESH_PRI_1MHZ_LOC", 0),
        ("CONFIG_MM_EXPERIMENTAL_MESH_PREQ_INTERVAL_MS", 4000),
        ("IP_STACK", "lwip"),
        ("MMIPAL_IPV4_ENABLED", 1),
        ("MMIPAL_IPV6_ENABLED", 0),
        ("MESH_PREQ_INTERVAL_MS", 4000),
        ("MMPKTMEM_TX_POOL_N_BLOCKS", 20),
        ("MMPKTMEM_RX_POOL_N_BLOCKS", 23),
        ("ON_DEMAND_TIMERS_ENABLED", 0),
    ],
    CCFLAGS=[
        "-Wno-c++-compat",
        "-Wno-unused-but-set-variable",
        "-Wno-unused-function",
        "-Wno-unused-parameter",
        "-Wno-unused-variable",
        "-Wno-format",
        "-Wno-maybe-uninitialized",
        "-Wno-dangling-else",
        "-Wno-type-limits",
        "-Wno-sign-compare",
        "-Wno-parentheses",
        "-Wno-packed-not-aligned",
        "-Wno-misleading-indentation",
        "-Wno-address",
    ],
)

objects = []
for src in shim_sources:
    rel = os.path.relpath(src, SDK_DIR)
    obj = os.path.join(OUT_DIR, "obj", rel + ".o")
    objects.extend(build_env.Object(obj, src))

archive = os.path.join(OUT_DIR, "libmm_iot_esp32.a")
archive_node = env.Command(archive, [libmorse] + objects, _build_archive)

fw_file = _get_define("CONFIG_MM_FW_FILE", "mm6108.mbin")
bcf_file = _get_define("CONFIG_MM_BCF_FILE", "bcf_mf08651_us.mbin")
fw_source = _find_blob(fw_file)
bcf_source = _find_blob(bcf_file)
fw_obj = env.Command(
    os.path.join(OUT_DIR, os.path.basename(fw_file) + ".o"),
    fw_source,
    _build_mbin_object,
    MM_PREFIX="firmware_binary",
)
bcf_obj = env.Command(
    os.path.join(OUT_DIR, os.path.basename(bcf_file) + ".o"),
    bcf_source,
    _build_mbin_object,
    MM_PREFIX="bcf_binary",
)

env.Prepend(CPPPATH=include_dirs)
env.Prepend(LIBPATH=[OUT_DIR])
env.Append(LIBS=["mm_iot_esp32"], LINKFLAGS=[str(fw_obj[0]), str(bcf_obj[0])])
env.Depends(env.subst("$BUILD_DIR/${PROGNAME}.elf"), [archive_node, fw_obj, bcf_obj])
