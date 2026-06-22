#!/usr/bin/env python3
"""
Prepare the Morse HaLow Zephyr module for the nRF54L15 DK PlatformIO build.
"""
Import("env")

import os
import shutil
import subprocess
from pathlib import Path

PROJECT_DIR = Path(env.subst("$PROJECT_DIR")).resolve()
BUILD_DIR = Path(env.subst("$BUILD_DIR")).resolve()
module_root = (PROJECT_DIR / "third_party" / "mm-iot-zephyr").resolve()
fallback_roots = [
    Path(os.environ["MMIOT_ZEPHYR_ROOT"]).expanduser().resolve() if "MMIOT_ZEPHYR_ROOT" in os.environ else None,
    (PROJECT_DIR / ".." / "mm-iot-zephyr").resolve(),
    (PROJECT_DIR / ".." / "edge-device-nrf54" / "modules" / "mm-iot-zephyr").resolve(),
]

(BUILD_DIR / "zephyr").mkdir(parents=True, exist_ok=True)


def is_complete_morse_module(path):
    return (
        path.exists()
        and (path / "CMakeLists.txt").exists()
        and (path / "components" / "morse_sm" / "hostap" / "morse_mbedtls_config.h").exists()
    )


mm_root = module_root
if not is_complete_morse_module(mm_root):
    mm_root = next((root for root in fallback_roots if root is not None and is_complete_morse_module(root)), module_root)

if not is_complete_morse_module(mm_root):
    print("ERROR: Morse Zephyr module is missing.")
    print("Initialize third_party/mm-iot-zephyr or set MMIOT_ZEPHYR_ROOT to a complete checkout.")
    env.Exit(1)


def first_existing(paths):
    for path in paths:
        if path.exists():
            return path
    return None


env.setdefault("ENV", {})
env["ENV"]["MORSE_ZEPHYR_MODULE"] = str(mm_root)
os.environ["MORSE_ZEPHYR_MODULE"] = str(mm_root)

project_bcf = PROJECT_DIR / "bcf_HC01.mbin"
blob_dir = mm_root / "zephyr" / "blobs"
staged_files = [
    (
        first_existing(
            [
                mm_root / "zephyr" / "blobs" / "lib" / "mm6108" / "arm-cortex-m33f" / "libmorse.a",
                mm_root / "submodules" / "mm-iot-sdk" / "framework" / "morselib" / "lib" / "arm-cortex-m33f" / "libmorse.a",
            ]
        ),
        blob_dir / "lib" / "mm6108" / "arm-cortex-m33f" / "libmorse.a",
        "libmorse.a",
    ),
    (
        first_existing(
            [
                mm_root / "zephyr" / "blobs" / "firmware" / "mm6108.mbin",
                mm_root / "submodules" / "mm-iot-sdk" / "framework" / "morsefirmware" / "mm6108.mbin",
            ]
        ),
        blob_dir / "firmware" / "mm6108.mbin",
        "mm6108.mbin",
    ),
    (
        first_existing(
            [
                project_bcf,
                mm_root / "zephyr" / "blobs" / "firmware" / "bcf_HC01.mbin",
                mm_root / "submodules" / "mm-iot-sdk" / "framework" / "morsefirmware" / "mm6108" / "bcfs" / "bcf_HC01.mbin",
            ]
        ),
        blob_dir / "firmware" / "bcf_HC01.mbin",
        "bcf_HC01.mbin",
    ),
]

for src, dst, label in staged_files:
    if src is None:
        print(f"ERROR: {label} not found in project or Morse module checkout")
        env.Exit(1)
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not dst.exists() or src.read_bytes() != dst.read_bytes():
        shutil.copyfile(src, dst)
        print(f"Staged {label}: {dst}")

sdk_bcf = mm_root / "submodules" / "mm-iot-sdk" / "framework" / "morsefirmware" / "mm6108" / "bcfs" / "bcf_HC01.mbin"
staged_bcf = blob_dir / "firmware" / "bcf_HC01.mbin"
sdk_bcf_src = project_bcf if project_bcf.exists() else staged_bcf
if sdk_bcf_src.exists():
    sdk_bcf.parent.mkdir(parents=True, exist_ok=True)
    if not sdk_bcf.exists() or sdk_bcf_src.read_bytes() != sdk_bcf.read_bytes():
        shutil.copyfile(sdk_bcf_src, sdk_bcf)
        print(f"Staged HC01 BCF for SDK path: {sdk_bcf}")

env["ENV"]["MORSE_SM_USE_APP_BINARIES"] = "1"
os.environ["MORSE_SM_USE_APP_BINARIES"] = "1"

print(f"Using Morse Zephyr module: {mm_root}")


def ensure_zephyr_final_linker_script(target, source, env):
    linker = BUILD_DIR / "zephyr" / "linker.cmd"
    if linker.exists():
        return

    build_ninja = BUILD_DIR / "build.ninja"
    lines = build_ninja.read_text(encoding="utf-8").splitlines()
    for index, line in enumerate(lines):
        if line.startswith("build zephyr/linker.cmd "):
            for command_line in lines[index + 1 :]:
                if command_line.startswith("  COMMAND = "):
                    subprocess.run(command_line.removeprefix("  COMMAND = "), shell=True, check=True)
                    return
            break

    raise RuntimeError(f"Could not find linker.cmd generation command in {build_ninja}")


for suffix in ("", ".elf"):
    env.AddPreAction(str(BUILD_DIR / f"{env.subst('$PROGNAME')}{suffix}"), ensure_zephyr_final_linker_script)
