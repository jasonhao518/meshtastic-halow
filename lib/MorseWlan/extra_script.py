"""
Build the EdgeZ HaLow wrapper from the precompiled SDK archive and local
Meshtastic shims.
"""

import os
import re
import subprocess
import ssl
import shutil
import zipfile
import urllib.request

Import("env")

PROJECT_DIR = env.subst("$PROJECT_DIR")
BUILD_DIR = env.subst("$BUILD_DIR")
LIBMORSE_DIR = os.path.join(PROJECT_DIR, "lib", "MorseWlan", "lib", "esp32-xtensa-lx7")
LOCAL_SOURCE_DIR = os.path.join(PROJECT_DIR, "lib", "MorseWlan", "src")
LOCAL_INCLUDE_DIR = os.path.join(PROJECT_DIR, "lib", "MorseWlan", "include")
HALOW_INCLUDE_LOCAL = os.path.join(LIBMORSE_DIR, "include")
HALOW_LIB_VERSION = "v0.0.3"
HALOW_LIB_NAME = "libedgez-esp32s3.a"
HALOW_LIB_ZIP_NAME = "libedgez-esp32s3.zip"
HALOW_LIB_URL = (
    "https://github.com/edgez-ai/halow-sdk/releases/download/"
    f"{HALOW_LIB_VERSION}/{HALOW_LIB_ZIP_NAME}"
)
HALOW_LIB_ZIP_LOCAL = os.path.join(LIBMORSE_DIR, HALOW_LIB_ZIP_NAME)
HALOW_LIB_LOCAL = os.path.join(LIBMORSE_DIR, HALOW_LIB_NAME)
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


def _define_enabled(name):
    return _get_define(name, "0").lower() in ("1", "y", "yes", "true", "on")


def _find_blob(name):
    basename = os.path.basename(name)
    roots = [
        os.path.join(PROJECT_DIR, "morsefirmware"),
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
    return os.path.join(PROJECT_DIR, "lib", "MorseWlan", "src", basename)


def _binary_symbol(path, suffix):
    return "_binary_" + re.sub(r"[^A-Za-z0-9_]", "_", path) + "_" + suffix


def _run(cmd, **kwargs):
    print(" ".join(cmd))
    subprocess.check_call(cmd, **kwargs)


def _toolchain_program(suffix):
    ar = env.subst("$AR")
    prefix = re.sub(r"(gcc-)?ar$", "", ar)
    return prefix + suffix


def _download_file(url, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    print(f"Downloading {url}")
    try:
        urllib.request.urlretrieve(url, path)
        return
    except Exception as first_err:
        print(f"Secure download failed ({first_err}); retrying with fallback methods.")

    try:
        with urllib.request.urlopen(url, context=ssl._create_unverified_context(), timeout=120) as response:
            with open(path, "wb") as out_file:
                out_file.write(response.read())
        return
    except Exception as fallback_err:
        print(f"Unverified HTTPS download failed ({fallback_err}); trying curl.")

    curl = shutil.which("curl")
    if not curl:
        raise
    _run([curl, "--fail", "--location", "--show-error", "--silent", "--output", path, url])


def _ensure_halow_archive():
    if not os.path.isfile(HALOW_LIB_ZIP_LOCAL):
        _download_file(HALOW_LIB_URL, HALOW_LIB_ZIP_LOCAL)

    if os.path.isdir(HALOW_INCLUDE_LOCAL):
        shutil.rmtree(HALOW_INCLUDE_LOCAL, ignore_errors=True)
    os.makedirs(HALOW_INCLUDE_LOCAL, exist_ok=True)

    with zipfile.ZipFile(HALOW_LIB_ZIP_LOCAL, "r") as zf:
        found_lib = False
        found_include = False
        fallback_lib_path = None
        for member in zf.infolist():
            if member.is_dir():
                continue

            normalized = member.filename.lstrip("./").replace("\\", "/")
            if not normalized:
                continue

            if normalized.endswith("/"):
                continue

            if os.path.basename(normalized) == HALOW_LIB_NAME:
                with zf.open(member) as source, open(HALOW_LIB_LOCAL, "wb") as out:
                    out.write(source.read())
                found_lib = True
            elif os.path.basename(normalized).endswith(".a") and fallback_lib_path is None:
                fallback_lib_path = (zf, member)

            rel_include = None
            if normalized.startswith("include/"):
                rel_include = normalized[len("include/"):]
            elif f"/include/" in f"/{normalized}":
                rel_include = normalized.split("/include/", 1)[1]

            if rel_include is None or not rel_include:
                continue

            out_path = os.path.join(HALOW_INCLUDE_LOCAL, rel_include)
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            with zf.open(member) as source, open(out_path, "wb") as out:
                out.write(source.read())
            found_include = True

        if (not found_lib) and fallback_lib_path is not None:
            zip_file, fallback_member = fallback_lib_path
            with zip_file.open(fallback_member) as source, open(HALOW_LIB_LOCAL, "wb") as out:
                out.write(source.read())
            found_lib = True

        if not found_lib:
            members = [m.filename for m in zf.infolist()]
            raise RuntimeError(
                f"Could not find {HALOW_LIB_NAME} in {HALOW_LIB_ZIP_LOCAL}; entries: {members}"
            )
        if not found_include:
            raise RuntimeError("Could not find include payload in " + HALOW_LIB_ZIP_LOCAL)


def _ensure_mmap_h_compat():
    compat_path = os.path.join(HALOW_INCLUDE_LOCAL, "mmipal.h")
    if os.path.exists(compat_path):
        return

    os.makedirs(HALOW_INCLUDE_LOCAL, exist_ok=True)
    with open(compat_path, "w", encoding="utf-8") as out:
        out.write("""/*
 * Compatibility shim for missing mmipal.h from the extracted EdgeZ archive.
 *
 * The public Morphe SDK release bundle for ESP32-S3 currently ships with
 * headers sufficient for most symbols except mmipal.h. This local header
 * preserves the API used by Meshtastic's lwIP shim.
 */
#ifndef MMIPAL_H
#define MMIPAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define MMIPAL_IPADDR_STR_MAXLEN 46

enum mmipal_status {
    MMIPAL_SUCCESS = 0,
    MMIPAL_NO_MEM = 1,
    MMIPAL_NO_LINK = 2,
    MMIPAL_INVALID_ARGUMENT = 3,
    MMIPAL_NOT_SUPPORTED = 4
};

enum mmipal_link_state {
    MMIPAL_LINK_DOWN = 0,
    MMIPAL_LINK_UP = 1
};

enum mmipal_addr_mode {
    MMIPAL_DISABLED = 0,
    MMIPAL_DHCP = 1,
    MMIPAL_DHCP_OFFLOAD = 2,
    MMIPAL_STATIC = 3,
    MMIPAL_AUTOIP = 4
};

enum mmipal_ip6_addr_mode {
    MMIPAL_IP6_DISABLED = 0,
    MMIPAL_IP6_STATIC = 1,
    MMIPAL_IP6_AUTOCONFIG = 2,
    MMIPAL_IP6_DHCP6_STATELESS = 3
};

typedef char mmipal_ip_addr_t[MMIPAL_IPADDR_STR_MAXLEN];
#define MMIPAL_IPV6_ADDR_COUNT 3

struct mmipal_ip_config {
    enum mmipal_addr_mode mode;
    mmipal_ip_addr_t ip_addr;
    mmipal_ip_addr_t netmask;
    mmipal_ip_addr_t gateway_addr;
};

struct mmipal_ip6_config {
    enum mmipal_ip6_addr_mode ip6_mode;
    char ip6_addr[MMIPAL_IPV6_ADDR_COUNT][MMIPAL_IPADDR_STR_MAXLEN];
};

struct mmipal_link_status {
    enum mmipal_link_state link_state;
    mmipal_ip_addr_t ip_addr;
    mmipal_ip_addr_t netmask;
    mmipal_ip_addr_t gateway;
};

typedef void (*mmipal_link_status_cb_fn_t)(const struct mmipal_link_status *status);
typedef void (*mmipal_ext_link_status_cb_fn_t)(const struct mmipal_link_status *status, void *arg);

struct mmipal_init_args {
    enum mmipal_addr_mode mode;
    mmipal_ip_addr_t ip_addr;
    mmipal_ip_addr_t netmask;
    mmipal_ip_addr_t gateway_addr;
    bool offload_arp_response;
    uint32_t offload_arp_refresh_s;
    enum mmipal_ip6_addr_mode ip6_mode;
    mmipal_ip_addr_t ip6_addr;
};

enum mmipal_status mmipal_get_ip_config(struct mmipal_ip_config *config);
enum mmipal_status mmipal_set_ip_config(const struct mmipal_ip_config *config);
enum mmipal_status mmipal_get_ip_broadcast_addr(mmipal_ip_addr_t broadcast_addr);
enum mmipal_status mmipal_get_ip6_config(struct mmipal_ip6_config *config);
enum mmipal_status mmipal_set_ip6_config(const struct mmipal_ip6_config *config);
void mmipal_set_link_status_callback(mmipal_link_status_cb_fn_t fn);
void mmipal_set_ext_link_status_callback(mmipal_ext_link_status_cb_fn_t fn, void *arg);
enum mmipal_status mmipal_init(const struct mmipal_init_args *args);
void mmipal_get_link_packet_counts(uint32_t *tx_packets, uint32_t *rx_packets);
void mmipal_set_tx_qos_tid(uint8_t tid);
enum mmipal_link_state mmipal_get_link_state(void);
enum mmipal_status mmipal_get_local_addr(mmipal_ip_addr_t local_addr, const mmipal_ip_addr_t dest_addr);
enum mmipal_status mmipal_set_dns_server(uint8_t index, const mmipal_ip_addr_t addr);
enum mmipal_status mmipal_get_dns_server(uint8_t index, mmipal_ip_addr_t addr);

#ifdef __cplusplus
}
#endif

#endif /* MMIPAL_H */
""")


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


def _glob_c(root):
    matches = []
    for dirpath, _, filenames in os.walk(root):
        for filename in filenames:
            if filename.endswith(".c"):
                matches.append(os.path.join(dirpath, filename))
    return sorted(matches)


_ensure_halow_archive()
libmorse = HALOW_LIB_LOCAL
print(f"Using Morse archive: {libmorse}")

include_dirs = [LOCAL_INCLUDE_DIR, LOCAL_SOURCE_DIR]
include_dirs.append(HALOW_INCLUDE_LOCAL)
_ensure_mmap_h_compat()

build_env = env.Clone()
build_env.Prepend(CPPPATH=include_dirs)
build_env.Append(
    CPPDEFINES=[
        "CONFIG_IEEE80211AH",
        "CONFIG_AP",
        "CONFIG_AUTOSCAN",
        "CONFIG_AUTOSCAN_EXPONENTIAL",
        "CONFIG_BGSCAN",
        "CONFIG_BGSCAN_SIMPLE",
        "CONFIG_ECC",
        "CONFIG_FIPS",
        "MM_IOT",
        "CONFIG_MESH",
        "CONFIG_NO_ACCOUNTING",
        "CONFIG_NO_BSS_TRANS_MGMT",
        "CONFIG_NO_CONFIG_BLOBS",
        "CONFIG_NO_CONFIG_WRITE",
        "CONFIG_NO_RADIUS",
        "CONFIG_NO_RANDOM_POOL",
        "CONFIG_NO_RC4",
        "CONFIG_NO_ROBUST_AV",
        "CONFIG_NO_RRM",
        "CONFIG_NO_VLAN",
        "CONFIG_OPENSSL_INTERNAL_AES_WRAP",
        "CONFIG_OWE",
        "CONFIG_S1G_TWT",
        "CONFIG_SAE",
        "CONFIG_SHA256",
        "CONFIG_SHA384",
        "CONFIG_SME",
        "CONFIG_WNM",
        "IEEE8021X_EAPOL",
        "MAX_NUM_MLD_LINKS=1",
        "MAX_NUM_MLO_LINKS=1",
        ("CONFIG_MMHAL_CHIP_TYPE_MM6108", 1),
        ("CONFIG_MMHAL_CHIP_TYPE_MM8108", 0),
        ("CONFIG_MM_EXPERIMENTAL_MESH", 1),
        ("CONFIG_MM_EXPERIMENTAL_MESH_OP_CLASS", 1),
        ("CONFIG_MM_EXPERIMENTAL_MESH_CHAN", 27),
        ("CONFIG_MM_EXPERIMENTAL_MESH_PRI_1MHZ_LOC", 0),
        ("CONFIG_MM_EXPERIMENTAL_MESH_PREQ_INTERVAL_MS", 4000),
        ("CONFIG_MM_MESHTASTIC_DISCOVERY_ONLY", 1),
        ("MESH_PREQ_INTERVAL_MS", 4000),
        ("MMPKTMEM_TX_POOL_N_BLOCKS", 20),
        ("MMPKTMEM_RX_POOL_N_BLOCKS", 23),
        ("MMLOG_LEVEL_DEFAULT", 1),
        "NEED_AP_MLME",
        ("ON_DEMAND_TIMERS_ENABLED", 0),
        "OS_NO_C_LIB_DEFINES",
        ("WPA_SUPPLICANT_CLEANUP_INTERVAL", 120),
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

shim_sources = _glob_c(LOCAL_SOURCE_DIR)
objects = []
for src in shim_sources:
    rel = os.path.relpath(src, PROJECT_DIR)
    obj = os.path.join(OUT_DIR, "obj", rel + ".o")
    objects.extend(build_env.Object(obj, src))

libmorse_node = [libmorse]

archive = os.path.join(OUT_DIR, "libmm_iot_esp32.a")
archive_node = env.Command(archive, list(libmorse_node) + objects, _build_archive)

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
