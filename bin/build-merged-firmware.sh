#!/usr/bin/env bash

set -euo pipefail

if [ $# -lt 2 ]; then
  echo "Usage: $0 <pio_env> <platform>"
  echo "Example: $0 heltec-hc33-halow esp32s3"
  exit 1
fi

PIO_ENV=$1
PLATFORM=$2
VERSION=$(bin/buildinfo.py long)
BUILDDIR=".pio/build/$PIO_ENV"
OUTDIR=release
BASE="firmware-${PIO_ENV}-${VERSION}"
OUTFILE="${BASE}-merged.zip"
WEBIMAGE="${OUTDIR}/${BASE}-web.bin"
WEBIMAGE_APP_ONLY="${OUTDIR}/${BASE}-web-app.bin"
FLASH_SIZE_MB=8

MANIFEST="${BUILDDIR}/${BASE}.mt.json"
FACTORY_BIN="${BUILDDIR}/${BASE}.factory.bin"
APP_BIN="${BUILDDIR}/${BASE}.bin"
BOOTLOADER_BIN="${BUILDDIR}/bootloader.bin"
PARTITION_BIN="${BUILDDIR}/partitions.bin"
NVS_BIN="${BUILDDIR}/nvs.bin"
LITTLEFS_BIN="${OUTDIR}/littlefs-${PIO_ENV}-${VERSION}.bin"
if echo "$PIO_ENV" | grep -q "esp32c6"; then
  MCU="esp32c6"
elif echo "$PIO_ENV" | grep -q "esp32c3"; then
  MCU="esp32c3"
elif echo "$PIO_ENV" | grep -q "esp32s3"; then
  MCU="esp32s3"
else
  MCU="esp32"
fi

if [[ "$PLATFORM" != esp32* ]]; then
  echo "Platform '$PLATFORM' is not an ESP32 target; creating merged package only."
  ./bin/build-firmware.sh "$PIO_ENV" "$PLATFORM"
  rm -rf "${OUTDIR}/.merged_package/"
  mkdir -p "${OUTDIR}/.merged_package"
  cp "${BUILDDIR}/${BASE}"* "${OUTDIR}/.merged_package/" 2>/dev/null || true
  cp "${OUTDIR}/"*.mt.json "${OUTDIR}/.merged_package/" 2>/dev/null || true
  pushd "${OUTDIR}/.merged_package" >/dev/null
  rm -f "../$OUTFILE"
  zip -9 -r "../$OUTFILE" .
  popd >/dev/null
  echo "Created ${OUTDIR}/${OUTFILE}"
  exit 0
fi

./bin/build-firmware.sh "$PIO_ENV" "$PLATFORM"

if [ ! -f "$FACTORY_BIN" ]; then
  echo "Missing factory image: $FACTORY_BIN"
  exit 1
fi

detect_esptool() {
  if command -v python3 >/dev/null 2>&1 && python3 -m esptool version >/dev/null 2>&1; then
    ESPTOOL_CMD=(python3 -m esptool)
  elif command -v esptool >/dev/null 2>&1; then
    ESPTOOL_CMD=(esptool)
  elif command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL_CMD=(esptool.py)
  else
    return 1
  fi
}

manifest_value() {
  local expr="$1"
  if [ ! -f "$MANIFEST" ] || ! command -v jq >/dev/null 2>&1; then
    return 1
  fi
  local v
  v=$(jq -r "$expr" "$MANIFEST" 2>/dev/null || true)
  if [ -z "$v" ] || [ "$v" = "null" ]; then
    return 1
  fi
  echo "$v"
}

read_flash_size_mb() {
  local json_size
  json_size="$(python3 - "$MANIFEST" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

parts = data.get("part") if isinstance(data, dict) else None
if not isinstance(parts, list):
    sys.exit(0)

def parse_num(v):
    if v is None:
        return None
    if isinstance(v, int):
        return v
    if isinstance(v, str):
        s = v.strip().lower()
        try:
            return int(s, 0)
        except Exception:
            return None
    return None

max_end = 0
found = False
for p in parts:
    off = parse_num(p.get("offset"))
    size = parse_num(p.get("size"))
    if off is None or size is None:
        continue
    found = True
    end = off + size
    if end > max_end:
        max_end = end

if found:
    print(max_end)
PY
)"
  if [ -z "$json_size" ]; then
    return 1
  fi

  local size_bytes
  size_bytes=$json_size
  if [ "$size_bytes" -gt 0 ]; then
    local ceil_mb=$(( (size_bytes + 1048575) / 1048576 ))
    if [ "$ceil_mb" -le 0 ]; then
      return 1
    fi
    echo "${ceil_mb}"
    return 0
  fi
  return 1
}

if ! detect_esptool; then
  echo "esptool not found. Cannot build merged flash image."
  exit 1
else
  if [ -f "$MANIFEST" ]; then
    derived_size=$(read_flash_size_mb || true)
    if [ -n "${derived_size:-}" ] && [ "$derived_size" -gt 0 ]; then
      FLASH_SIZE_MB=$derived_size
    elif echo "$PIO_ENV" | grep -q "esp32s3"; then
      FLASH_SIZE_MB=8
    elif echo "$PIO_ENV" | grep -q "esp32c6"; then
      FLASH_SIZE_MB=4
    else
      FLASH_SIZE_MB=4
    fi
  fi

  manifest_mcu=$(manifest_value '.mcu' || true)
  if [ -n "${manifest_mcu}" ]; then
    MCU="$manifest_mcu"
  fi

  OTA_BIN="${OUTDIR}/mt-${MCU}-ota.bin"
  APP_OFFSET=$(manifest_value '[.part // [] | map(select(.subtype=="app0" or .name=="app0" or .subtype=="app" or .name=="app") | .offset) | .[0]' || echo "0x10000")
  SPIFFS_OFFSET=$(manifest_value '[.part // [] | map(select(.subtype=="spiffs" or .name=="spiffs" or .name=="littlefs") | .offset) | .[0]' || echo "0x300000")
  OTA_OFFSET=$(manifest_value '[.part // [] | map(select(.subtype=="ota_1") | .offset) | .[0]' || echo "0x260000")
  BOOT_OFFSET=$(manifest_value '[.part // [] | map(select(.subtype=="bootloader" or .name=="bootloader") | .offset) | .[0]' || echo "0x1000")
  PART_OFFSET=$(manifest_value '[.part // [] | map(select(.subtype=="partition" or .name=="partition-table" or .name=="partitions" or .subtype=="partition-table") | .offset) | .[0]' || echo "0x8000")
  NVS_OFFSET=$(manifest_value '[.part // [] | map(select(.subtype=="nvs" or .name=="nvs") | .offset) | .[0]' || echo "0x9000")

  MERGE_ARGS=()
  if [ -f "$BOOTLOADER_BIN" ]; then
    MERGE_ARGS+=("$BOOT_OFFSET" "$BOOTLOADER_BIN")
  fi
  if [ -f "$PARTITION_BIN" ]; then
    MERGE_ARGS+=("$PART_OFFSET" "$PARTITION_BIN")
  fi
  if [ -f "$NVS_BIN" ]; then
    MERGE_ARGS+=("$NVS_OFFSET" "$NVS_BIN")
  fi
  if [ -f "$APP_BIN" ]; then
    MERGE_ARGS+=("$APP_OFFSET" "$APP_BIN")
  else
    echo "Warning: app bin missing; using factory image only."
    cp "$FACTORY_BIN" "$WEBIMAGE"
    MERGE_ARGS=()
  fi
  if [ -f "$OTA_BIN" ]; then
    MERGE_ARGS+=("$OTA_OFFSET" "$OTA_BIN")
  fi
  if [ -f "$LITTLEFS_BIN" ]; then
    MERGE_ARGS+=("$SPIFFS_OFFSET" "$LITTLEFS_BIN")
  fi

  if [ ${#MERGE_ARGS[@]} -gt 0 ]; then
    if [ -n "${MCU}" ]; then
      MERGE_CMD=("${ESPTOOL_CMD[@]}" --chip "$MCU" merge_bin)
    else
      MERGE_CMD=("${ESPTOOL_CMD[@]}" --chip auto merge_bin)
    fi
    if "${MERGE_CMD[@]}" \
      --flash_size "${FLASH_SIZE_MB}MB" \
      --fill-flash-size "${FLASH_SIZE_MB}MB" \
      --output "$WEBIMAGE" \
      "${MERGE_ARGS[@]}"; then
      echo "Created merged web image: $WEBIMAGE"
    else
      echo "esptool merge_bin failed."
      exit 1
    fi
  else
    echo "No inputs for merge_bin."
    exit 1
  fi
fi

if [ ! -f "$WEBIMAGE" ]; then
  echo "Failed to create web image."
  exit 1
fi

WEB_SIZE_BYTES=$(wc -c < "$WEBIMAGE")
echo "Merged web image size: ${WEB_SIZE_BYTES} bytes (target flash ${FLASH_SIZE_MB}MB)"

if [ -f "$APP_BIN" ]; then
  cp "$APP_BIN" "$WEBIMAGE_APP_ONLY"
else
  echo "App partition binary not found; app-only web image was not generated."
fi

rm -rf "${OUTDIR}/.merged_package/"
mkdir -p "${OUTDIR}/.merged_package"

cp "$WEBIMAGE" "${OUTDIR}/.merged_package/" || exit 1
cp "${BUILDDIR}/${BASE}.elf" "${OUTDIR}/.merged_package/" || true
cp "${BUILDDIR}/${BASE}.bin" "${OUTDIR}/.merged_package/" || true
cp "$FACTORY_BIN" "${OUTDIR}/.merged_package/" || true
cp "$MANIFEST" "${OUTDIR}/.merged_package/" || true
cp "$LITTLEFS_BIN" "${OUTDIR}/.merged_package/" || true
cp "$OUTDIR"/littlefs-"$PIO_ENV"-"$VERSION".bin "${OUTDIR}/.merged_package/" 2>/dev/null || true
cp "$OUTDIR"/mt-"$MCU"-ota.bin "${OUTDIR}/.merged_package/" 2>/dev/null || true
cp "$WEBIMAGE_APP_ONLY" "${OUTDIR}/.merged_package/" 2>/dev/null || true
cp bin/device-install.sh bin/device-install.bat bin/device-update.sh bin/device-update.bat "${OUTDIR}/.merged_package/" 2>/dev/null || true

for f in "${OUTDIR}/"*.mt.json "${OUTDIR}/firmware-${PIO_ENV}-${VERSION}"*; do
  cp "$f" "${OUTDIR}/.merged_package/" 2>/dev/null || true
done

if ls "${BUILDDIR}"/*.merged.hex >/dev/null 2>&1; then
  cp "${BUILDDIR}"/*.merged.hex "${OUTDIR}/.merged_package/" || true
fi

pushd "${OUTDIR}/.merged_package" >/dev/null
rm -f "../$OUTFILE"
zip -9 -r "../$OUTFILE" .
popd >/dev/null

echo "Created ${OUTDIR}/${OUTFILE}"
